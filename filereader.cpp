#include "filereader.h"

FileReader::FileReader()
    : file(nullptr), cursor(0)
{
}

FileReader::~FileReader()
{
    close();
}

bool FileReader::open(const char* path)
{
    file = std::fopen(path, "rb+"); // rb+ allows reading + writing
    if (!file)
        return false;

    cursor = 0;
    return true;
}

void FileReader::close()
{
    if (file) {
        std::fclose(file);
        file = nullptr;
    }
    cursor = 0;
}

uint8_t FileReader::next()
{
    if (!file)
        return 0;

    int c = std::fgetc(file);
    if (c == EOF)
        return 0;

    cursor++;
    return static_cast<uint8_t>(c);
}

void FileReader::advance(size_t count)
{
    if (!file)
        return;

    std::fseek(file, count, SEEK_CUR);
    cursor += count;
}

void FileReader::seek(size_t pos)
{
    if (!file)
        return;

    std::fseek(file, pos, SEEK_SET);
    cursor = pos;
}

void FileReader::set(uint8_t value)
{
    if (!file)
        return;

    std::fputc(value, file);
    std::fflush(file);

    cursor++;
}

size_t FileReader::pos() const
{
    return cursor;
}

bool FileReader::eof() const
{
    if (!file)
        return true;

    return std::feof(file);
}