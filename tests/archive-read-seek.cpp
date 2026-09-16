/*
	Kirikiroid2 SDL2 port - archive read/seek verifier.

	Opens an archive through the engine's own storage layer (TVPOpenArchive,
	i.e. exactly what the game loader uses) and checks, member by member, that
	the stream the archive hands out is byte-exact:

	  * the reported size equals the original file's size
	  * a full sequential read matches a CRC32 of the original file
	  * seeks (to the middle, to the tail and backwards again) read the same
	    bytes the original file holds at those offsets
	  * a backwards seek followed by a full re-read still matches, i.e. the
	    stream is not a one-shot forward scan
	  * reading at end of stream returns 0 rather than over-reading past the
	    member's end

	Built and run by tests/run-archive-read-seek.sh, which takes the compiler
	flags and the link libraries from the CMake build tree.

	usage: archive-read-seek <archive> <member>:<original-file> [...]

	<member> is an in-archive storage name ('/' or '\\' separators, any case);
	it is normalized the way the engine normalizes a requested name before
	handing it to the archive, and the same upper-case spelling is additionally
	resolved through TVPCreateStream() to show the engine's own lookup works.
*/

#include "tjsCommHead.h"
#include "StorageIntf.h"

#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

// The engine's storage layer only resolves absolute paths (main.cpp does the
// same with realpath() before handing the game path to the engine).
std::string absolutePath(const std::string &path)
{
	char resolved[4096];
	if (realpath(path.c_str(), resolved)) return std::string(resolved);
	return path;
}

int gPass = 0;
int gFail = 0;

void check(bool ok, const std::string &what)
{
	if (ok) {
		gPass++;
		printf("PASS %s\n", what.c_str());
	} else {
		gFail++;
		printf("FAIL %s\n", what.c_str());
	}
}

// tTVPArchive::CreateStream() looks the name up in a hash built from the
// archive's *normalized* names and does not normalize its argument: the engine
// normalizes the in-archive part first (TVPCreateStream() calls
// NormalizeInArchiveStorageName() before the lookup), and every backend -
// ZIP, TAR, XP3 and this one - relies on that.  This verifier asks the archive
// object directly, so it normalizes the member names the same way, and the
// checks below that go through TVPCreateStream() additionally show that the
// engine's own case-insensitive lookup finds the same member from an
// upper-case, non-normalized name.
std::string normalizedInArchiveName(const std::string &name)
{
	std::string r = name;
	for (char &c : r) {
		if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
		else if (c == '\\') c = '/';
	}
	return r;
}

// CRC32 of a whole file, straight from the OS.
uLong fileCrc(const std::string &path, tjs_uint64 *sizeOut)
{
	std::ifstream f(path, std::ios::binary);
	if (!f) return 0;
	std::vector<char> buf(65536);
	uLong crc = crc32(0L, Z_NULL, 0);
	tjs_uint64 total = 0;
	while (f) {
		f.read(buf.data(), buf.size());
		std::streamsize n = f.gcount();
		if (n <= 0) break;
		crc = crc32(crc, reinterpret_cast<const Bytef *>(buf.data()), (uInt)n);
		total += (tjs_uint64)n;
	}
	if (sizeOut) *sizeOut = total;
	return crc;
}

// CRC32 of [offset, offset+length) of a file, read through a seek.
uLong fileSliceCrc(const std::string &path, tjs_uint64 offset, tjs_uint64 length,
	tjs_uint64 *gotOut, bool *eofShort)
{
	std::ifstream f(path, std::ios::binary);
	if (!f) return 0;
	f.seekg((std::streamoff)offset);
	std::vector<char> buf((size_t)length);
	f.read(buf.data(), (std::streamsize)length);
	std::streamsize n = f.gcount();
	if (n < 0) n = 0;
	if (gotOut) *gotOut = (tjs_uint64)n;
	if (eofShort) *eofShort = ((tjs_uint64)n < length);
	return crc32(0L, reinterpret_cast<const Bytef *>(buf.data()), (uInt)n);
}

tjs_uint64 streamCrc(tTJSBinaryStream *st, tjs_uint64 *sizeOut)
{
	st->Seek(0, TJS_BS_SEEK_SET);
	std::vector<char> buf(8192);
	uLong crc = crc32(0L, Z_NULL, 0);
	tjs_uint64 total = 0;
	for (;;) {
		tjs_uint n = st->Read(buf.data(), (tjs_uint)buf.size());
		if (!n) break;
		crc = crc32(crc, reinterpret_cast<const Bytef *>(buf.data()), (uInt)n);
		total += n;
	}
	if (sizeOut) *sizeOut = total;
	return crc;
}

// Reads exactly `length` bytes at `offset` through the archive stream; returns
// what the stream delivered and its CRC.
uLong streamSliceCrc(tTJSBinaryStream *st, tjs_uint64 offset, tjs_uint64 length,
	tjs_uint64 *gotOut)
{
	st->Seek((tjs_int64)offset, TJS_BS_SEEK_SET);
	std::vector<char> buf((size_t)length);
	tjs_uint64 got = 0;
	while (got < length) {
		tjs_uint n = st->Read(buf.data() + got, (tjs_uint)(length - got));
		if (!n) break;
		got += n;
	}
	if (gotOut) *gotOut = got;
	return crc32(0L, reinterpret_cast<const Bytef *>(buf.data()), (uInt)got);
}

std::string upperAscii(const std::string &s)
{
	std::string r = s;
	for (char &c : r) {
		if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
	}
	return r;
}

void verifyMember(tTVPArchive *arc, const std::string &member, const std::string &original)
{
	// The member argument is given in storage-name form, whatever its case;
	// the archive object wants it normalized (see normalizedInArchiveName).
	const std::string entry = normalizedInArchiveName(member);
	printf("-- member '%s'%s vs '%s'\n", member.c_str(),
		entry == member ? "" : (" (looked up as '" + entry + "')").c_str(),
		original.c_str());

	tjs_uint64 fileSize = 0;
	uLong fileCrcValue = fileCrc(original, &fileSize);
	if (!fileSize) {
		check(false, member + ": original file is empty/unreadable");
		return;
	}

	tTJSBinaryStream *st = nullptr;
	try {
		st = arc->CreateStream(ttstr(entry));
	} catch (...) {
		st = nullptr;
	}
	if (!st) {
		check(false, member + ": CreateStream returned nothing");
		return;
	}

	check(st->GetSize() == fileSize,
		member + ": GetSize=" + std::to_string(st->GetSize())
		+ " original=" + std::to_string(fileSize));

	// full sequential read
	tjs_uint64 readSize = 0;
	uLong readCrc = streamCrc(st, &readSize);
	check(readSize == fileSize,
		member + ": full read delivered " + std::to_string(readSize)
		+ " bytes (original " + std::to_string(fileSize) + ")");
	check(readCrc == fileCrcValue,
		member + ": full read crc32=" + std::to_string(readCrc)
		+ " original=" + std::to_string(fileCrcValue));

	// forward seek into the middle (the slice is clamped to what is left, so
	// this also works for members smaller than the slice)
	const tjs_uint64 sliceSize = 4096;
	const tjs_uint64 mid = fileSize / 3;
	const tjs_uint64 midLen = std::min(sliceSize, fileSize - mid);
	{
		tjs_uint64 got = 0;
		uLong want = fileSliceCrc(original, mid, midLen, nullptr, nullptr);
		uLong have = streamSliceCrc(st, mid, midLen, &got);
		check(got == midLen && have == want,
			member + ": read " + std::to_string(midLen) + " bytes at offset "
			+ std::to_string(mid) + " crc32=" + std::to_string(have)
			+ " original=" + std::to_string(want));
	}

	// seek from the end (the engine does this to find a file's size / read
	// RIFF or MP4 tail structures)
	const tjs_uint64 tailLen = std::min(sliceSize / 2, fileSize);
	const tjs_uint64 tailStart = fileSize - tailLen;
	{
		tjs_uint64 pos = st->Seek(-(tjs_int64)tailLen, TJS_BS_SEEK_END);
		tjs_uint64 got = 0;
		uLong want = fileSliceCrc(original, tailStart, tailLen, nullptr, nullptr);
		uLong have = streamSliceCrc(st, tailStart, tailLen, &got);
		check(pos == tailStart,
			member + ": Seek(-" + std::to_string(tailLen) + ", END) -> "
			+ std::to_string(pos) + ", expected " + std::to_string(tailStart));
		check(got == tailLen && have == want,
			member + ": last " + std::to_string(tailLen) + " bytes crc32="
			+ std::to_string(have) + " original=" + std::to_string(want));
	}

	// backwards seek: a forward-only reader fails here
	const tjs_uint64 backStart = std::min((tjs_uint64)17, fileSize / 2);
	const tjs_uint64 backLen = std::min(sliceSize, fileSize - backStart);
	{
		tjs_uint64 got = 0;
		uLong want = fileSliceCrc(original, backStart, backLen, nullptr, nullptr);
		uLong have = streamSliceCrc(st, backStart, backLen, &got);
		check(got == backLen && have == want,
			member + ": backwards seek to " + std::to_string(backStart) + ", read "
			+ std::to_string(backLen) + " crc32=" + std::to_string(have)
			+ " original=" + std::to_string(want));
	}

	// and a full re-read after that backwards seek
	{
		tjs_uint64 readSize2 = 0;
		uLong readCrc2 = streamCrc(st, &readSize2);
		check(readSize2 == fileSize && readCrc2 == fileCrcValue,
			member + ": re-read after backwards seek delivered "
			+ std::to_string(readSize2) + " bytes crc32=" + std::to_string(readCrc2));
	}

	// past the end
	{
		char buf[16];
		st->Seek(0, TJS_BS_SEEK_END);
		tjs_uint n = st->Read(buf, sizeof(buf));
		check(n == 0, member + ": read at end of stream returned " + std::to_string(n)
			+ " bytes (expected 0)");
	}

	delete st;
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <archive> <member>:<original-file> [...]\n", argv[0]);
		return 2;
	}

	std::vector<std::pair<std::string, std::string> > members;
	for (int i = 2; i < argc; i++) {
		std::string a = argv[i];
		size_t colon = a.find(':');
		if (colon == std::string::npos) {
			fprintf(stderr, "bad argument '%s', want <member>:<original-file>\n", argv[i]);
			return 2;
		}
		members.push_back(std::make_pair(a.substr(0, colon), a.substr(colon + 1)));
	}

	const std::string archivePath = absolutePath(argv[1]);

	tTVPArchive *arc = nullptr;
	try {
		arc = TVPOpenArchive(ttstr(archivePath), true);
	} catch (...) {
		arc = nullptr;
	}
	if (!arc) {
		printf("FAIL open: TVPOpenArchive returned nothing for '%s'\n", argv[1]);
		printf("SUMMARY pass=0 fail=1\n");
		return 1;
	}

	check(arc->GetCount() > 0,
		std::string("open: archive holds ") + std::to_string(arc->GetCount()) + " entries");
	for (tjs_uint i = 0; i < arc->GetCount(); i++) {
		printf("     entry %u: %s (%s)\n", i, arc->GetName(i).AsStdString().c_str(),
			arc->IsExistent(arc->GetName(i)) ? "found by name" : "NOT findable");
	}

	// A game asks the engine for "Tone.WAV" whatever case the archive stored:
	// the engine normalises the in-archive part of the name and looks it up.
	// This goes through TVPCreateStream (i.e. the storage layer + the archive
	// cache), the exact path the script engine uses.
	for (size_t i = 0; i < members.size(); i++) {
		const std::string &m = members[i].first;
		std::string upper = upperAscii(m);
		tjs_uint64 want = 0;
		fileCrc(members[i].second, &want);
		tTJSBinaryStream *st = nullptr;
		try {
			st = TVPCreateStream(ttstr(archivePath + ">" + upper));
		} catch (...) {
			st = nullptr;
		}
		check(st != nullptr && want && st->GetSize() == want,
			m + ": engine storage lookup of '" + upper + "' -> "
			+ (st ? std::to_string(st->GetSize()) : std::string("not found"))
			+ " bytes (expected " + std::to_string(want) + ")");
		delete st;
	}

	for (size_t i = 0; i < members.size(); i++) {
		verifyMember(arc, members[i].first, members[i].second);
	}

	delete arc;

	printf("SUMMARY pass=%d fail=%d\n", gPass, gFail);
	fflush(stdout);

	// _Exit(), not return: the checks above populate the engine's archive
	// cache, whose teardown is registered as a static destructor, while the
	// archive-handle pool's shutdown is registered as a plain atexit handler
	// in another translation unit.  In the krkr2 binary those handlers run in
	// the safe order; in this verifier (a different link order) the cache's
	// destructor can run first and dead-lock on the pool's critical section,
	// which would hang the verifier *after* it has done all its work.
	// Exit-time teardown is not what this tool verifies.
	std::_Exit(gFail ? 1 : 0);
}
