#pragma once

#include <cstdint>
#include <cstdlib>
#include <ctype.h>
#include <string>


#if defined (_MSC_VER)
#   pragma warning(disable:4996)	// The POSIX name for this item is deprecated. (some warning microsoft made up on a whim, based on a gross misunderstanding of POSIX standards, and which nothing else adheres to)
#   define strdup   _strdup
#endif


inline char* strchr_ajek(const char* src, char delim)
{
    if (!src || !src[0]) return nullptr;
    while(src[0] && src[0] != delim) { ++src; }
    return (char*)src;
}

inline char* strtok_ajek(char* (&curr), char* (&next), char delim)
{
    if (!curr) return nullptr;
    if (!curr[0]) return nullptr;

    next = strchr_ajek(curr, delim);

    char* begg = curr;
    char* endd = next-1;

    while (                  isspace((uint8_t)begg[0])) {              ++begg; }
    while ((endd >= begg) && isspace((uint8_t)endd[0])) { endd[0] = 0; --endd; }

    curr = next + (next[0] ? 1 : 0);
    next[0] = 0;	// delete comma delimiter

    if (endd >= begg) {
        return begg;
    }
    return nullptr;
}

struct StringTokenizer
{
    ~StringTokenizer() {
        if (m_string) {
            free(m_string);
            m_string = nullptr;
        }
    }

    char*			m_string;

    char*			m_curr	= nullptr;
    char*			m_next	= nullptr;

    uint8_t			m_lastDelim = 0;

    const char*	GetNextToken	(uint8_t delim=0);
    const char* GetNextTokenTrim(uint8_t delim=0);
    uint8_t		GetLastDelim	() const			{ return m_lastDelim; }
};

inline StringTokenizer Tokenizer(const char* src) {
    auto* dup = strdup(src);
    return { dup, dup };
}

inline StringTokenizer Tokenizer(const std::string& src) {
    auto* dup = strdup(src.c_str());
    return { dup, dup };
}

inline const char* StringTokenizer::GetNextToken(uint8_t delim)
{
    m_lastDelim = m_next ? m_next[0] : 0;
    return strtok_ajek(m_curr, m_next, delim);
}

inline const char* StringTokenizer::GetNextTokenTrim(uint8_t delim)
{
    m_lastDelim = m_next ? m_next[0] : 0;
    if (uint8_t* result = (uint8_t*)strtok_ajek(m_curr, m_next, delim)) {
        auto* next = (uint8_t*)m_next;
        while (result < next  && isblank(*result))   { ++result; }
        while (next >= result && isblank(next[-1]))  { --next; }
        next[0] = 0;
        return (char*)result;
    }
    return nullptr;
}
