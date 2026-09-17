#pragma once

#include <string_view>
#include <vector>

enum class TokenKind { eKeyword, eNumber, eString, eComment, eFunction, eDirective };

struct SyntaxToken
{
    long start = 0;
    long end = 0;
    TokenKind kind = TokenKind::eKeyword;
};

// Offsets use UTF-16 code units, matching Windows Rich Edit ranges.
std::vector<SyntaxToken> scanSyntax(std::wstring_view source);
