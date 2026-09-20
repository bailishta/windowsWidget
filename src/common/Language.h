#pragma once
#include "Common.h"
namespace ww {
inline std::string normalize_language(std::string_view locale) {
    return locale.size() >= 2 && (locale[0] == 'z' || locale[0] == 'Z') &&
                   (locale[1] == 'h' || locale[1] == 'H')
               ? "zh-CN"
               : "en-US";
}
inline std::string default_language() {
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE ? "zh-CN" : "en-US";
}
inline void set_thread_language(std::string_view language) {
    SetThreadUILanguage(language == "zh-CN" ? MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED)
                                            : MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
}
inline bool thread_chinese() {
    return PRIMARYLANGID(GetThreadUILanguage()) == LANG_CHINESE;
}
inline const wchar_t *thread_locale() {
    return thread_chinese() ? L"zh-CN" : L"en-US";
}
inline const wchar_t *localized(bool chinese, const wchar_t *zh, const wchar_t *en) {
    return chinese ? zh : en;
}
} // namespace ww
