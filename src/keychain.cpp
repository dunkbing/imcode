#include "keychain.hpp"

#if defined(__APPLE__)
#include <Security/Security.h>

namespace Keychain {
    namespace {
        // a query for the generic password of service/account; the caller releases it
        CFMutableDictionaryRef query(const std::string& service, const std::string& account) {
            CFMutableDictionaryRef q = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                                 &kCFTypeDictionaryValueCallBacks);
            CFStringRef s = CFStringCreateWithCString(nullptr, service.c_str(), kCFStringEncodingUTF8);
            CFStringRef a = CFStringCreateWithCString(nullptr, account.c_str(), kCFStringEncodingUTF8);
            CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
            CFDictionarySetValue(q, kSecAttrService, s);
            CFDictionarySetValue(q, kSecAttrAccount, a);
            CFRelease(s);
            CFRelease(a);
            return q;
        }
    } // namespace

    std::string read(const std::string& service, const std::string& account) {
        CFMutableDictionaryRef q = query(service, account);
        CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
        CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
        CFTypeRef found = nullptr;
        std::string out;
        if (SecItemCopyMatching(q, &found) == errSecSuccess && found) {
            const auto data = static_cast<CFDataRef>(found);
            out.assign(reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
                       static_cast<size_t>(CFDataGetLength(data)));
            CFRelease(found);
        }
        CFRelease(q);
        return out;
    }

    bool write(const std::string& service, const std::string& account, const std::string& secret) {
        remove(service, account);
        CFMutableDictionaryRef q = query(service, account);
        CFDataRef data = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(secret.data()),
                                      static_cast<CFIndex>(secret.size()));
        CFDictionarySetValue(q, kSecValueData, data);
        CFDictionarySetValue(q, kSecAttrLabel, CFSTR("ImCode ssh sign-in"));
        const bool ok = SecItemAdd(q, nullptr) == errSecSuccess;
        CFRelease(data);
        CFRelease(q);
        return ok;
    }

    void remove(const std::string& service, const std::string& account) {
        CFMutableDictionaryRef q = query(service, account);
        SecItemDelete(q);
        CFRelease(q);
    }
} // namespace Keychain
#else
namespace Keychain {
    std::string read(const std::string&, const std::string&) {
        return "";
    }
    bool write(const std::string&, const std::string&, const std::string&) {
        return false;
    }
    void remove(const std::string&, const std::string&) {}
} // namespace Keychain
#endif
