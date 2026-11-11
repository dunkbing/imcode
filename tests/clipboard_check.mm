// a png put on a pasteboard of its own comes back as a file; the system clipboard is not touched
#include "../src/clipboard.hpp"
#import <AppKit/AppKit.h>
#include <cstdio>

int main(int argc, char** argv) {
    if (argc != 2) {
        return 2; // a png file
    }
    NSPasteboard* board = [NSPasteboard pasteboardWithUniqueName];
    [board clearContents];
    NSData* png = [NSData dataWithContentsOfFile:@(argv[1])];
    [board setData:png forType:NSPasteboardTypePNG];
    const auto dir = std::filesystem::temp_directory_path() / "imcode-clipboard-check";
    const auto files = clipboardFiles(dir, board.name.UTF8String);
    const bool ok = files.size() == 1 && std::filesystem::file_size(files[0]) == png.length;
    // nothing on it: nothing comes back
    [board clearContents];
    const bool empty = clipboardFiles(dir, board.name.UTF8String).empty();
    [board releaseGlobally];
    std::filesystem::remove_all(dir);
    std::printf(ok && empty ? "ok\n" : "FAILED: %zu files\n", files.size());
    return ok && empty ? 0 : 1;
}
