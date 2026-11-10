#pragma once

#include <cstddef>

// a shell's bytes both ways: a pty on this machine (terminal.cpp) or an ssh channel
// (ssh.cpp). read on one thread; write, resize and close from any other
struct Pty {
    virtual ~Pty() = default;
    // blocks; <= 0 once the shell is gone or close() was called
    virtual long read(char* buf, size_t size) = 0;
    virtual bool write(const char* data, size_t size) = 0;
    virtual void resize(int cols, int rows) = 0;
    virtual void close() = 0; // ends a read that is waiting
};
