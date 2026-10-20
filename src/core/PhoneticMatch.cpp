#include "core/PhoneticMatch.h"

#include <algorithm>
#include <vector>

namespace ake::PhoneticMatch {

int allowedDistance(int length)
{
    if (length < kMinLength) return -1;
    return length >= 7 ? 2 : 1;
}

bool isKanaOnly(const QString& reading)
{
    if (reading.isEmpty()) return false;
    for (QChar c : reading) {
        const ushort u = c.unicode();
        if (!((u >= 0x3041 && u <= 0x3096) || u == 0x30FC || u == 0x309D || u == 0x309E)) return false;
    }
    return true;
}

int distance(const QString& pattern, const QString& text, int maxDistance)
{
    const int n = int(pattern.size());
    if (n == 0) return 0;
    if (text.isEmpty()) return std::min(n, maxDistance + 1);
    // Sellers: edit distance with a free start and a free end in the text. prev[i] = best distance of pattern[0..i) ending here.
    std::vector<int> prev(n + 1), cur(n + 1);
    for (int i = 0; i <= n; ++i) prev[i] = i;
    int best = n;
    for (int j = 0; j < text.size(); ++j) {
        cur[0] = 0;                                            // a match may start anywhere
        for (int i = 1; i <= n; ++i)
            cur[i] = std::min({ prev[i] + 1, cur[i - 1] + 1, prev[i - 1] + (pattern[i - 1] != text[j] ? 1 : 0) });
        best = std::min(best, cur[n]);
        if (best == 0) break;
        std::swap(prev, cur);
    }
    return std::min(best, maxDistance + 1);
}

} // namespace ake::PhoneticMatch
