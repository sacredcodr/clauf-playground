#include "syntax.hpp"

#include <algorithm>
#include <cwctype>

namespace
{
bool identifierStart(wchar_t character)
{
    return std::iswalpha(character) || character == L'_';
}

bool identifierPart(wchar_t character)
{
    return identifierStart(character) || std::iswdigit(character);
}
}

std::vector<SyntaxToken> scanSyntax(std::wstring_view source)
{
    static constexpr std::wstring_view keywords[] = {
        L"alignas", L"alignof", L"auto", L"bool", L"break", L"case", L"char", L"const",
        L"constexpr", L"continue", L"default", L"do", L"double", L"else", L"enum", L"extern",
        L"false", L"float", L"for", L"goto", L"if", L"inline", L"int", L"long", L"nullptr",
        L"register", L"restrict", L"return", L"short", L"signed", L"sizeof", L"static",
        L"static_assert", L"struct", L"switch", L"thread_local", L"true", L"typedef",
        L"typeof", L"typeof_unqual", L"union", L"unsigned", L"void", L"volatile", L"while",
        L"_Alignas", L"_Alignof", L"_Atomic", L"_BitInt", L"_Bool", L"_Complex", L"_Generic",
        L"_Imaginary", L"_Noreturn", L"_Static_assert", L"_Thread_local"
    };
    std::vector<SyntaxToken> tokens;
    size_t position = 0;
    while (position < source.size())
    {
        const size_t start = position;
        const wchar_t character = source[position];
        const wchar_t next = position + 1 < source.size() ? source[position + 1] : L'\0';
        TokenKind kind;
        if (character == L'/' && (next == L'/' || next == L'*'))
        {
            kind = TokenKind::eComment;
            position += 2;
            if (next == L'/')
            {
                while (position < source.size() && source[position] != L'\r' && source[position] != L'\n') { ++position; }
            }
            else
            {
                const size_t close = source.find(L"*/", position);
                position = close == source.npos ? source.size() : close + 2;
            }
        }
        else if (character == L'\"' || character == L'\'')
        {
            kind = TokenKind::eString;
            ++position;
            while (position < source.size())
            {
                const wchar_t current = source[position++];
                if (current == L'\\' && position < source.size()) { ++position; }
                else if (current == character || current == L'\r' || current == L'\n') { break; }
            }
        }
        else if (std::iswdigit(character) || (character == L'.' && std::iswdigit(next)))
        {
            kind = TokenKind::eNumber;
            ++position;
            const bool hexadecimal = character == L'0' && (next == L'x' || next == L'X');
            while (position < source.size())
            {
                const wchar_t current = source[position];
                const wchar_t previous = source[position - 1];
                const bool exponent = hexadecimal ? previous == L'p' || previous == L'P' : previous == L'e' || previous == L'E';
                if (identifierPart(current) || current == L'.' || current == L'\'' ||
                    ((current == L'+' || current == L'-') && exponent)) { ++position; }
                else { break; }
            }
        }
        else if (identifierStart(character))
        {
            ++position;
            while (position < source.size() && identifierPart(source[position])) { ++position; }
            const auto word = source.substr(start, position - start);
            if (std::find(std::begin(keywords), std::end(keywords), word) != std::end(keywords))
            {
                kind = TokenKind::eKeyword;
            }
            else
            {
                size_t following = position;
                while (following < source.size() && std::iswspace(source[following])) { ++following; }
                if (following == source.size() || source[following] != L'(') { continue; }
                kind = TokenKind::eFunction;
            }
        }
        else if (character == L'#')
        {
            kind = TokenKind::eDirective;
            ++position;
            while (position < source.size() && (source[position] == L' ' || source[position] == L'\t')) { ++position; }
            while (position < source.size() && identifierPart(source[position])) { ++position; }
        }
        else
        {
            ++position;
            continue;
        }
        tokens.push_back({static_cast<long>(start), static_cast<long>(position), kind});
    }
    return tokens;
}
