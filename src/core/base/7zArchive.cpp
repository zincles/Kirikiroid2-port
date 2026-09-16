/*
	Kirikiroid2 SDL2 port - 7z archive backend, built on libarchive.

	Kirikiroid2 supports .7z as a game archive.  The 7-Zip *SDK* is not part of
	this port (it lived in the vendor/ tree, which does not exist here), so the
	format is read through libarchive instead: it is already a link dependency
	of both targets and already unpacks archives elsewhere in the engine, and
	it reads 7z (LZMA/LZMA2/BCJ/PPMd, solid or not).

	libarchive has no per-entry random-access API: a reader is a sequential
	cursor over an archive which can only move forward, or be re-opened and
	start over.  The engine, however, wants one tTJSBinaryStream per in-archive
	file with working GetSize()/Seek() and short reads at end of stream - the
	sound, movie and text readers all seek (RIFF and MP4 scanning seeks to the
	end of the stream and back).

	Each stream returned by CreateStreamByIndex() therefore owns a *private*
	archive_read handle over a private file handle, positioned on its own
	member:

	  * the archive is enumerated once at open.  That is a header walk only:
	    7z keeps its metadata at the end of the file, and no member data is
	    decompressed to list the entries.
	  * opening a member stream scans that private reader forward to the
	    member's header ordinal - O(entries) header steps, again without
	    decompressing any member data.
	  * reads pull the member's data blocks as libarchive produces them.  In a
	    solid 7z block the members stored before this one in the same block are
	    decompressed and discarded first; that cost is paid once per stream,
	    not once per read.
	  * a forward Seek() discards data blocks up to the target offset.
	  * a backward Seek() marks the reader for a re-open: the next read
	    re-scans to the member (O(entries) headers) and decompresses up to the
	    target again.  Readers that seek once and then read sequentially (the
	    normal engine pattern) pay nothing extra; a caller that ping-pongs
	    between two offsets pays the scan per backward seek.  For the archive
	    sizes games ship this is milliseconds, because the re-scan itself
	    decompresses nothing.

	Nothing is cached in memory: buffering whole members is what the ZIP
	backend does for compressed entries, and it is exactly what must not
	happen for a game archive's movie and sound members, which are the large
	ones.
*/

#include "tjsCommHead.h"
#include "StorageIntf.h"
#include "MsgIntf.h"
#include <algorithm>
#include <vector>

extern "C" {
#include "libarchive/archive.h"
#include "libarchive/archive_entry.h"
}

// The archive file-handle pool (XP3Archive.cpp): keeps one open handle per
// archive so a stream can reach the archive's data without re-resolving the
// storage name.  Declared here the same way TARArchive.cpp declares its part.
tTJSBinaryStream * TVPGetCachedArchiveHandle(void * pointer, const ttstr & name);
void TVPReleaseCachedArchiveHandle(void * pointer, tTJSBinaryStream * stream);
void TVPFreeArchiveHandlePoolByPointer(void * pointer);

// From TARArchive.cpp: narrow (UTF-8) archive entry name -> ttstr.
void storeFilename(ttstr &name, const char *narrowName, const ttstr &filename);

namespace {

// 7z signature: "7z" followed by 0xBC 0xAF 0x27 0x1C.
const unsigned char SevenZipSignature[6] = { 0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C };

//---------------------------------------------------------------------------
// SevenZipReader
//
// One libarchive reader over one stream.  libarchive never owns the stream:
// the archive object and each member stream manage the handle's lifetime
// (the archive object hands its handle back to the pool, a member stream gets
// one from the pool and releases it when it is destroyed).
//---------------------------------------------------------------------------
class SevenZipReader
{
public:
	struct archive *Arc;
	tTJSBinaryStream *Input;
	// Buffer handed to libarchive's read callback.  Allocated on the first
	// read rather than with the stream, so a stream that is created but never
	// read costs nothing.
	std::vector<tjs_uint8> Buffer;

	SevenZipReader() : Arc(nullptr), Input(nullptr) {}
	~SevenZipReader() { Close(); }

	bool Open(tTJSBinaryStream *input) {
		Close();
		// libarchive detects the format by reading from the current position,
		// so the handle must start at the beginning of the archive.
		input->SetPosition(0);
		Input = input;
		Arc = archive_read_new();
		if (!Arc) {
			Input = nullptr;
			return false;
		}
		if (Buffer.empty()) Buffer.resize(64 * 1024);
		archive_read_support_filter_all(Arc);
		archive_read_support_format_7zip(Arc);
		// 7z stores its metadata at the end of the file and seeks back and
		// forth over it; without the seek callback every jump would be a
		// read-and-discard.
		archive_read_set_seek_callback(Arc, SeekCallback);
		if (archive_read_open2(Arc, this, nullptr, ReadCallback, SkipCallback, nullptr) != ARCHIVE_OK) {
			Close();
			return false;
		}
		return true;
	}

	void Close() {
		if (Arc) {
			archive_read_close(Arc);
			archive_read_free(Arc);
			Arc = nullptr;
		}
		Input = nullptr;
	}

	// Move the reader to the given header ordinal (0 based, counting every
	// header in the archive, including the ones the archive object skipped).
	// Header-only: no member data is decompressed by this.
	bool GotoOrdinal(tjs_uint ordinal) {
		struct archive_entry *entry;
		for (tjs_uint i = 0; i <= ordinal; i++) {
			if (archive_read_next_header(Arc, &entry) != ARCHIVE_OK) return false;
		}
		return true;
	}

private:
	static la_ssize_t ReadCallback(struct archive *, void *client_data, const void **buff) {
		SevenZipReader *self = static_cast<SevenZipReader *>(client_data);
		*buff = self->Buffer.data();
		return (la_ssize_t)self->Input->Read(self->Buffer.data(), (tjs_uint)self->Buffer.size());
	}

	static la_int64_t SkipCallback(struct archive *, void *client_data, la_int64_t request) {
		SevenZipReader *self = static_cast<SevenZipReader *>(client_data);
		tjs_uint64 before = self->Input->GetPosition();
		tjs_uint64 after = self->Input->Seek(request, TJS_BS_SEEK_CUR);
		return (la_int64_t)(after - before);
	}

	static la_int64_t SeekCallback(struct archive *, void *client_data, la_int64_t offset, int whence) {
		SevenZipReader *self = static_cast<SevenZipReader *>(client_data);
		return (la_int64_t)self->Input->Seek(offset, whence);
	}
};

//---------------------------------------------------------------------------
// SevenZipArchive
//---------------------------------------------------------------------------
class SevenZipArchive;

struct SevenZipEntry
{
	ttstr Name;
	tjs_uint64 Size;
	tjs_uint Ordinal; // header ordinal inside the archive
};

class SevenZipArchive : public tTVPArchive
{
	typedef std::vector<SevenZipEntry> tFileList;
	tFileList FileList;

public:
	SevenZipArchive(const ttstr & name) : tTVPArchive(name) {}
	~SevenZipArchive() { TVPFreeArchiveHandlePoolByPointer(this); }

	// Enumerates the archive through `st` (borrowed: it is only handed to the
	// handle pool once the archive is accepted).
	bool Init(tTJSBinaryStream *st, bool normalizeFileName);

	virtual tjs_uint GetCount() { return FileList.size(); }
	virtual ttstr GetName(tjs_uint idx) { return FileList[idx].Name; }
	virtual tTJSBinaryStream * CreateStreamByIndex(tjs_uint idx);

	const SevenZipEntry & GetEntry(tjs_uint idx) const { return FileList[idx]; }
};

//---------------------------------------------------------------------------
// SevenZipArchiveStream
//
// The stream the engine gets for one archive member.
//---------------------------------------------------------------------------
class SevenZipArchiveStream : public tTJSBinaryStream
{
	SevenZipArchive *Owner;
	tjs_uint Ordinal;
	tjs_uint64 Size;
	tjs_uint64 Pos;

	SevenZipReader Reader;
	tTJSBinaryStream *Input; // archive handle while the reader is open
	const tjs_uint8 *Block;  // current data block (owned by libarchive)
	tjs_uint64 BlockOffset;  // ... and its uncompressed offset in the member
	tjs_uint64 BlockSize;

	bool AtEnd;       // the reader reported the end of this member's data
	bool NeedRestart; // the reader is not positioned on this member / this offset
	bool Broken;      // re-opening the reader failed: report an empty stream

	bool InCurrentBlock(tjs_uint64 pos) const {
		return Block != nullptr && pos >= BlockOffset && pos < BlockOffset + BlockSize;
	}

	bool Reopen() {
		Reader.Close();
		ReleaseInput();
		Input = TVPGetCachedArchiveHandle(Owner, Owner->ArchiveName);
		if (!Input || !Reader.Open(Input) || !Reader.GotoOrdinal(Ordinal)) {
			Reader.Close();
			ReleaseInput();
			Broken = true;
			NeedRestart = false;
			AtEnd = true;
			return false;
		}
		Block = nullptr;
		BlockOffset = BlockSize = 0;
		AtEnd = false;
		NeedRestart = false;
		return true;
	}

	void ReleaseInput() {
		if (Input) {
			TVPReleaseCachedArchiveHandle(Owner, Input);
			Input = nullptr;
		}
	}

	bool NextBlock() {
		for (;;) {
			const void *buff;
			size_t size;
			la_int64_t offset;
			int r = archive_read_data_block(Reader.Arc, &buff, &size, &offset);
			if (r == ARCHIVE_EOF || r < ARCHIVE_WARN) {
				AtEnd = true;
				return false;
			}
			if (size == 0) continue; // no progress possible from an empty block
			Block = static_cast<const tjs_uint8 *>(buff);
			BlockSize = (tjs_uint64)size;
			BlockOffset = (tjs_uint64)offset;
			return true;
		}
	}

	// Make the block holding Pos current, advancing or re-opening as needed.
	bool EnsureBlock() {
		if (NeedRestart && !Reopen()) return false;
		while (!AtEnd) {
			if (InCurrentBlock(Pos)) return true;
			if (Block && Pos < BlockOffset) {
				// only reachable after a backward seek: start over
				if (!Reopen()) return false;
				continue;
			}
			if (!NextBlock()) break;
		}
		return InCurrentBlock(Pos);
	}

public:
	SevenZipArchiveStream(SevenZipArchive *owner, tjs_uint ordinal, tjs_uint64 size)
		: Owner(owner), Ordinal(ordinal), Size(size), Pos(0), Input(nullptr),
		  Block(nullptr), BlockOffset(0), BlockSize(0), AtEnd(false),
		  NeedRestart(true), Broken(false) {}

	virtual ~SevenZipArchiveStream() {
		Reader.Close();
		ReleaseInput();
	}

	virtual tjs_uint64 TJS_INTF_METHOD Seek(tjs_int64 offset, int whence) {
		tjs_int64 next;
		switch (whence) {
		case TJS_BS_SEEK_SET: next = offset; break;
		case TJS_BS_SEEK_CUR: next = (tjs_int64)Pos + offset; break;
		case TJS_BS_SEEK_END: next = (tjs_int64)Size + offset; break;
		default: return Pos;
		}
		if (next < 0) next = 0;
		else if ((tjs_uint64)next > Size) next = (tjs_int64)Size;
		Pos = (tjs_uint64)next;
		// Data already decompressed past the new position cannot be rewound:
		// the next read has to re-scan from the member's start.
		if (!InCurrentBlock(Pos) && Pos < BlockOffset + BlockSize) NeedRestart = true;
		return Pos;
	}

	virtual tjs_uint TJS_INTF_METHOD Read(void *buffer, tjs_uint read_size) {
		tjs_uint8 *out = static_cast<tjs_uint8 *>(buffer);
		tjs_uint total = 0;
		while (total < read_size && Pos < Size) {
			if (!EnsureBlock()) break;
			tjs_uint64 available = BlockOffset + BlockSize - Pos;
			tjs_uint64 want = read_size - total;
			if (want > available) want = available;
			if (want > Size - Pos) want = Size - Pos;
			memcpy(out + total, Block + (Pos - BlockOffset), (size_t)want);
			Pos += want;
			total += (tjs_uint)want;
		}
		return total;
	}

	virtual tjs_uint TJS_INTF_METHOD Write(const void *, tjs_uint) { return 0; }

	virtual tjs_uint64 TJS_INTF_METHOD GetSize() { return Size; }
};

//---------------------------------------------------------------------------
bool SevenZipArchive::Init(tTJSBinaryStream *st, bool normalizeFileName)
{
	if (!st) return false;

	SevenZipReader reader;
	if (!reader.Open(st)) return false;

	// Walk the headers once.  A libarchive entry pointer is only valid until
	// the next header call, so everything the engine needs later (name, size,
	// header ordinal) is copied out here.
	struct archive_entry *entry;
	tjs_uint ordinal = 0;
	for (;;) {
		int r = archive_read_next_header(reader.Arc, &entry);
		if (r == ARCHIVE_EOF) break;
		if (r < ARCHIVE_WARN) {
			// FAILED/FATAL: a truncated file, a container libarchive cannot
			// read, or an encrypted 7z header (which needs a password this
			// engine has no way to ask for).  Decline the archive so the
			// remaining creators get their turn and the engine reports its
			// normal "cannot open" error instead of a game that fails
			// half-way through loading.
			reader.Close();
			return false;
		}
		if (r < ARCHIVE_OK) break; // WARN/RETRY: keep the entries seen so far
		if (archive_entry_is_encrypted(entry)) {
			// 7z AES-encrypted content: the entry list is readable but no
			// member can be decrypted (libarchive reports "the file content is
			// encrypted, but currently not supported" on the first read), so
			// the archive would enumerate and then serve empty files.
			reader.Close();
			return false;
		}
		const char *narrow = archive_entry_pathname_utf8(entry);
		if (narrow && *narrow && archive_entry_filetype(entry) != AE_IFDIR) {
			SevenZipEntry item;
			storeFilename(item.Name, narrow, ArchiveName);
			if (normalizeFileName)
				NormalizeInArchiveStorageName(item.Name);
			item.Size = (tjs_uint64)archive_entry_size(entry);
			item.Ordinal = ordinal;
			FileList.push_back(item);
		}
		ordinal++;
	}
	reader.Close();

	if (normalizeFileName) {
		std::sort(FileList.begin(), FileList.end(),
			[](const SevenZipEntry &a, const SevenZipEntry &b) {
				return a.Name < b.Name;
			});
	}

	// The archive is accepted: hand the handle to the pool, from which
	// CreateStreamByIndex()'s streams take one (and hand back) -
	// the same lifetime scheme the TAR and XP3 backends use.
	TVPReleaseCachedArchiveHandle(this, st);
	return true;
}

//---------------------------------------------------------------------------
tTJSBinaryStream * SevenZipArchive::CreateStreamByIndex(tjs_uint idx)
{
	if (idx >= FileList.size()) TVPThrowExceptionMessage(TVPReadError);
	const SevenZipEntry &item = FileList[idx];
	return new SevenZipArchiveStream(this, item.Ordinal, item.Size);
}

} // namespace

//---------------------------------------------------------------------------
// TVPOpen7ZArchive
//
// ArchiveCreators[] is tried in order, so this has to decide cheaply and leave
// the stream untouched for anything that is not a 7z archive: the 6 byte
// signature at the start of the file, the same kind of magic test the ZIP and
// TAR creators do, is the format probe.  A file that carries the signature but
// that libarchive cannot read - truncated, corrupt, or password protected -
// is declined too (see Init), so the remaining creators get their turn and the
// engine reports its normal "cannot open the game" error rather than starting
// a game whose files cannot be read.
//---------------------------------------------------------------------------
tTVPArchive * TVPOpen7ZArchive(const ttstr & name, tTJSBinaryStream *st, bool normalizeFileName)
{
	if (!st) return nullptr;

	tjs_uint64 pos = st->GetPosition();
	unsigned char signature[sizeof(SevenZipSignature)];
	tjs_uint read = st->Read(signature, sizeof(signature));
	st->SetPosition(pos);
	if (read != sizeof(signature) || memcmp(signature, SevenZipSignature, sizeof(signature)) != 0)
		return nullptr;

	SevenZipArchive *arc = new SevenZipArchive(name);
	if (!arc->Init(st, normalizeFileName)) {
		delete arc;
		return nullptr;
	}
	return arc;
}
