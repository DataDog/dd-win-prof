#pragma once

#include "pch.h"

typedef std::pair<std::string, std::string> tag;
typedef std::vector<tag> tags;

class TagsHelper
{
public:
    TagsHelper() = delete;
    ~TagsHelper() = delete;

    static tags Parse(std::string&& s);

private:
    static tag ParseTag(std::string_view s);
};

