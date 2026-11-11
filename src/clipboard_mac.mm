#include "clipboard.hpp"

#import <AppKit/AppKit.h>
#include <chrono>

std::vector<std::string> clipboardFiles(const std::filesystem::path& tempDir, const char* pasteboard) {
    std::vector<std::string> out;
    @autoreleasepool {
        NSPasteboard* board = pasteboard ? [NSPasteboard pasteboardWithName:@(pasteboard)]
                                         : [NSPasteboard generalPasteboard];
        // files copied in finder
        NSArray<NSURL*>* urls = [board readObjectsForClasses:@[ NSURL.class ]
                                                     options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
        for (NSURL* url in urls) {
            out.push_back(url.path.UTF8String);
        }
        if (!out.empty()) {
            return out;
        }
        // a picture: png as it is, anything else appkit can read (tiff from a screenshot) as png
        NSData* png = [board dataForType:NSPasteboardTypePNG];
        if (!png) {
            if (NSData* tiff = [board dataForType:NSPasteboardTypeTIFF]) {
                png = [[NSBitmapImageRep imageRepWithData:tiff] representationUsingType:NSBitmapImageFileTypePNG
                                                                             properties:@{}];
            }
        }
        if (!png) {
            return out;
        }
        std::error_code ec;
        std::filesystem::create_directories(tempDir, ec);
        const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
        const std::filesystem::path file = tempDir / ("pasted-" + std::to_string(stamp) + ".png");
        if ([png writeToFile:@(file.c_str()) atomically:YES]) {
            out.push_back(file.string());
        }
    }
    return out;
}
