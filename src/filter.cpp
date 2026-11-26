// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 MorpheusUK. All Rights Reserved.

#include "mrss/settings.hpp"
#define PCRE2_CODE_UNIT_WIDTH 16
#include <cwctype>
#include <memory>
#include <pcre2.h>
#include <windows.h>

namespace mrss
{
namespace
{
std::wstring fold(std::wstring_view v)
{
    std::wstring s(v);
    if (!s.empty())
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, v.data(), static_cast<int>(v.size()), s.data(),
                      static_cast<int>(s.size()), nullptr, nullptr, 0);
    return s;
}
class Boolean
{
    enum Kind
    {
        end,
        term,
        andOp,
        orOp,
        notOp,
        left,
        right
    };
    std::wstring source_, text_;
    std::size_t pos_ = 0;
    Kind kind_ = end;
    std::wstring word_;
    unsigned depth_ = 0;
    void token()
    {
        while (pos_ < source_.size() && iswspace(source_[pos_]))
            ++pos_;
        if (pos_ == source_.size())
        {
            kind_ = end;
            return;
        }
        const auto c = source_[pos_++];
        if (c == L'(')
        {
            kind_ = left;
            return;
        }
        if (c == L')')
        {
            kind_ = right;
            return;
        }
        word_.clear();
        bool quoted = c == L'"';
        if (quoted)
        {
            while (pos_ < source_.size() && source_[pos_] != L'"')
                word_ += source_[pos_++];
            if (pos_ == source_.size())
                throw std::runtime_error("Unclosed quoted phrase");
            ++pos_;
        }
        else
        {
            word_ += c;
            while (pos_ < source_.size() && !iswspace(source_[pos_]) && source_[pos_] != L'(' &&
                   source_[pos_] != L')')
                word_ += source_[pos_++];
        }
        if (word_.empty())
            throw std::runtime_error("Empty Boolean phrase");
        const auto op = fold(word_);
        kind_ = !quoted && op == L"and"   ? andOp
                : !quoted && op == L"or"  ? orOp
                : !quoted && op == L"not" ? notOp
                                          : term;
    }
    bool unary()
    {
        if (++depth_ > 64)
            throw std::runtime_error("Boolean expression is too deep");
        bool r = false;
        if (kind_ == notOp)
        {
            token();
            r = !unary();
        }
        else if (kind_ == left)
        {
            token();
            r = expression();
            if (kind_ != right)
                throw std::runtime_error("Missing closing parenthesis");
            token();
        }
        else if (kind_ == term)
        {
            r = text_.find(word_) != text_.npos;
            token();
        }
        else
            throw std::runtime_error("Expected a term, NOT or parenthesis");
        --depth_;
        return r;
    }
    bool conjunction()
    {
        bool r = unary();
        while (kind_ == andOp)
        {
            token();
            const auto b = unary();
            r = r && b;
        }
        return r;
    }
    bool expression()
    {
        bool r = conjunction();
        while (kind_ == orOp)
        {
            token();
            const auto b = conjunction();
            r = r || b;
        }
        return r;
    }

  public:
    Boolean(std::wstring_view s, std::wstring_view t, bool sensitive)
        : source_(sensitive ? std::wstring(s) : fold(s)), text_(sensitive ? std::wstring(t) : fold(t))
    {
        token();
    }
    bool run()
    {
        if (kind_ == end)
            return true;
        const auto r = expression();
        if (kind_ != end)
            throw std::runtime_error("Use AND or OR between terms");
        return r;
    }
};
bool regex(std::wstring_view pattern, std::wstring_view text, bool sensitive)
{
    int error = 0;
    PCRE2_SIZE offset = 0;
    std::unique_ptr<pcre2_code, decltype(&pcre2_code_free)> code(
        pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.data()), pattern.size(),
                      PCRE2_UTF | PCRE2_UCP | (sensitive ? 0 : PCRE2_CASELESS), &error, &offset, nullptr),
        pcre2_code_free);
    if (!code)
    {
        PCRE2_UCHAR message[180]{};
        pcre2_get_error_message(error, message, 180);
        throw std::runtime_error("Invalid regex at character " + std::to_string(offset) + ": " +
                                 utf8(reinterpret_cast<wchar_t *>(message)));
    }
    std::unique_ptr<pcre2_match_context, decltype(&pcre2_match_context_free)> context(
        pcre2_match_context_create(nullptr), pcre2_match_context_free);
    std::unique_ptr<pcre2_match_data, decltype(&pcre2_match_data_free)> data(
        pcre2_match_data_create_from_pattern(code.get(), nullptr), pcre2_match_data_free);
    if (!context || !data)
        throw std::runtime_error("Regex allocation failed");
    pcre2_set_match_limit(context.get(), 100000);
    pcre2_set_depth_limit(context.get(), 128);
    pcre2_set_heap_limit(context.get(), 1024);
    const auto result = pcre2_match(code.get(), reinterpret_cast<PCRE2_SPTR>(text.data()), text.size(), 0, 0,
                                    data.get(), context.get());
    if (result < 0 && result != PCRE2_ERROR_NOMATCH)
        throw std::runtime_error("Regex exceeded its work limit or contains unsupported text");
    return result >= 0;
}
} // namespace
bool matchesFilter(const Filter &f, std::wstring_view t)
{
    if (f.mode == FilterMode::none)
        return true;
    if (f.expression.size() > 2048 || f.exclude.size() > 2048 || t.size() > 262144)
        throw std::runtime_error("Filter or metadata exceeds limit");
    if (f.mode == FilterMode::boolean)
        return Boolean(f.expression, t, f.caseSensitive).run();
    return (f.expression.empty() || regex(f.expression, t, f.caseSensitive)) &&
           (f.exclude.empty() || !regex(f.exclude, t, f.caseSensitive));
}
void validateFilter(const Filter &f)
{
    (void)matchesFilter(f, L"validation sample");
}
} // namespace mrss
