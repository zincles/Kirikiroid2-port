#include "LocaleConfigManager.h"
#include "GlobalConfigManager.h"
#include "tinyxml2/tinyxml2.h"
#include "BinaryStream.h"
#include "StorageIntf.h"
#include "DebugIntf.h"
#if !defined(TVP_SDL2)
#include "ui/UIText.h"
#include "ui/UIButton.h"
#endif

LocaleConfigManager::LocaleConfigManager() {

}

std::string LocaleConfigManager::GetFilePath() {
	// The language file is a constant of the app package, so it is reached
	// through the engine's storage search paths instead of a fixed file name.
	// Returns the placed storage name, or an empty string when not even the
	// default language file is available.
	std::string fullpath = "locale/" + currentLangCode + ".xml"; // exp. "locale/en_us.xml"
	ttstr placed = TVPGetPlacedPath(ttstr(fullpath.c_str()));
	if (placed.IsEmpty()) {
		currentLangCode = "en_us"; // restore to default language config(must exist)
		fullpath = "locale/" + currentLangCode + ".xml";
		placed = TVPGetPlacedPath(ttstr(fullpath.c_str()));
	}
	return placed.AsNarrowStdString();
}

LocaleConfigManager* LocaleConfigManager::GetInstance() {
	static LocaleConfigManager instance;
	return &instance;
}

const std::string &LocaleConfigManager::GetText(const std::string &tid) {
	auto it = AllConfig.find(tid);
	if (it == AllConfig.end()) {
		AllConfig[tid] = tid;
		return AllConfig[tid];
	}
	return it->second;
}

void LocaleConfigManager::Initialize(const std::string &sysLang) {
	// override by global configured lang
	currentLangCode = GlobalConfigManager::GetInstance()->GetValue<std::string>("user_language", "");
	if (currentLangCode.empty()) currentLangCode = sysLang;
	AllConfig.clear();
	std::string path = GetFilePath();
	if (path.empty()) {
		// no locale file at all: GetText() then returns the message ids, the
		// same fallback the previous file utils based code produced for an
		// unreadable file.
		TVPAddLog(ttstr(("LocaleConfigManager: locale/" + currentLangCode + ".xml not found").c_str()));
		return;
	}
	std::string xmlData;
	{
		tTJSBinaryStream *stream = TVPCreateBinaryStreamForRead(ttstr(path.c_str()), TJS_W(""));
		tjs_uint64 size = stream->GetSize();
		xmlData.resize(static_cast<std::string::size_type>(size));
		if (size) stream->Read(&xmlData[0], static_cast<tjs_uint>(size));
		delete stream;
	}
	tinyxml2::XMLDocument doc;
	bool _writeBOM = false;
	const char* p = xmlData.c_str();
	p = tinyxml2::XMLUtil::ReadBOM(p, &_writeBOM);
	doc.Parse(p);
	tinyxml2::XMLElement *rootElement = doc.RootElement();
	if (rootElement) {
		for (tinyxml2::XMLElement *item = rootElement->FirstChildElement(); item; item = item->NextSiblingElement()) {
			const char *key = item->Attribute("id");
			const char *val = item->Attribute("text");
			if (key && val) {
				AllConfig[key] = val;
			}
		}
	}
}

#if !defined(TVP_SDL2)
bool LocaleConfigManager::initText(cocos2d::ui::Text *ctrl) {
	if (!ctrl) return false;
	return initText(ctrl, ctrl->getString());
}

bool LocaleConfigManager::initText(cocos2d::ui::Button *ctrl)
{
	if (!ctrl) return false;
	return initText(ctrl, ctrl->getTitleText());
}

bool LocaleConfigManager::initText(cocos2d::ui::Text *ctrl, const std::string &tid) {
	if (!ctrl) return false;

	std::string txt = GetText(tid);
	if (txt.empty()) {
		ctrl->setString(tid);
		ctrl->setColor(cocos2d::Color3B::RED);
		return false;
	}

	ctrl->setString(txt);
	return true;
}

bool LocaleConfigManager::initText(cocos2d::ui::Button *ctrl, const std::string &tid) {
	if (!ctrl) return false;

	std::string txt = GetText(tid);
	if (txt.empty()) {
		ctrl->setTitleText(tid);
		ctrl->setTitleColor(cocos2d::Color3B::RED);
		return false;
	}

	ctrl->setTitleText(txt);
	return true;
}
#endif // !defined(TVP_SDL2)
