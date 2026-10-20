#pragma once
#include <QString>

namespace ake {

// Approximate matching of phonetic forms (hiragana readings): how far a typed word is from the closest stretch of a text.
namespace PhoneticMatch {

constexpr int kMinLength = 4;      // shorter readings are not matched: one wrong kana would be a quarter of the word

// Edits allowed for a pattern of `length` kana: 0 = none (the pattern is too short for approximate matching, -1),
// 1 for 4..6, 2 from 7 (about a quarter of the length). Measured on a real recording: design.md "読みの近い語を拾う検索".
int allowedDistance(int length);

// Only a reading made of kana (and ー) is compared: it is measured for Japanese; Latin words and digits are not
// "read" by any dictionary, and a spelling tolerance for them was not evaluated.
bool isKanaOnly(const QString& reading);

// Smallest edit distance (insert, delete, substitute) between `pattern` and any substring of `text`;
// maxDistance + 1 when it is larger than maxDistance.
int distance(const QString& pattern, const QString& text, int maxDistance);

} // namespace PhoneticMatch

} // namespace ake
