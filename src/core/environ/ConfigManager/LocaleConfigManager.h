// multi language config mainly for ui
#pragma once
#include <unordered_map>
#include <string>
#include <vector>

#if !defined(TVP_SDL2)
// The cocos2d-x UI forms binding is not part of the SDL2 port; GetText() is
// what the port's UI code uses.
namespace cocos2d {
	namespace ui {
		class Text;
		class Button;
	}
}
#endif

class LocaleConfigManager {

	std::unordered_map<std::string, std::string> AllConfig; // tid->text in utf8

	bool ConfigUpdated;

	LocaleConfigManager();

	std::string GetFilePath();

public:
	static LocaleConfigManager* GetInstance();

	void Initialize(const std::string &sysLang);

	const std::string &GetText(const std::string &tid); // in utf8

#if !defined(TVP_SDL2)
	bool initText(cocos2d::ui::Text *ctrl);
	bool initText(cocos2d::ui::Button *ctrl);
	bool initText(cocos2d::ui::Text *ctrl, const std::string &tid);
	bool initText(cocos2d::ui::Button *ctrl, const std::string &tid);
#endif

private:

	std::string currentLangCode;
};
