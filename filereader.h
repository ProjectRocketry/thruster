#ifndef FILEREADER_HPP
#define FILEREADER_HPP

#include <cstdint>
#include <cstddef>
#include <cstdio>

class FileReader {
private:
    FILE*  file;
    size_t cursor;

public:
    FileReader();
    ~FileReader();

    bool open(const char* path);
    void close();

    uint8_t next();            // read next byte and advance
    void    advance(size_t count);
    void    seek(size_t pos);
    void    set(uint8_t value); // overwrite byte at current position
    size_t  pos() const;

    bool    eof() const;
};

#endif