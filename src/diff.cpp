#include "diff.hpp"

#include <algorithm>

namespace Diff {

    namespace {
        std::vector<std::string_view> split(std::string_view text) {
            std::vector<std::string_view> out;
            for (size_t at = 0; at < text.size();) {
                const size_t end = std::min(text.find('\n', at), text.size());
                out.push_back(text.substr(at, end - at));
                at = end + 1;
            }
            return out;
        }
    } // namespace

    std::vector<Line> lines(std::string_view before, std::string_view after, int context) {
        const auto a = split(before);
        const auto b = split(after);
        // the common head and tail cost nothing: edits are usually small
        size_t head = 0;
        while (head < a.size() && head < b.size() && a[head] == b[head]) {
            ++head;
        }
        size_t tail = 0;
        while (tail < a.size() - head && tail < b.size() - head &&
               a[a.size() - 1 - tail] == b[b.size() - 1 - tail]) {
            ++tail;
        }
        const size_t n = a.size() - head - tail;
        const size_t m = b.size() - head - tail;

        std::vector<Line> all;
        for (size_t i = 0; i < head; ++i) {
            all.push_back({' ', std::string(a[i])});
        }
        // shortcut: a table of n*m cells; past 4 million the middle is shown as all removed
        // then all added. a linear-space diff when whole rewritten files matter
        if (n * m <= 4'000'000) {
            std::vector<int> lcs((n + 1) * (m + 1), 0); // longest common run from (i, j) on
            for (size_t i = n; i-- > 0;) {
                for (size_t j = m; j-- > 0;) {
                    lcs[i * (m + 1) + j] = a[head + i] == b[head + j]
                                               ? lcs[(i + 1) * (m + 1) + j + 1] + 1
                                               : std::max(lcs[(i + 1) * (m + 1) + j],
                                                          lcs[i * (m + 1) + j + 1]);
                }
            }
            size_t i = 0, j = 0;
            while (i < n || j < m) {
                if (i < n && j < m && a[head + i] == b[head + j]) {
                    all.push_back({' ', std::string(a[head + i])});
                    ++i, ++j;
                } else if (j == m || (i < n && lcs[(i + 1) * (m + 1) + j] >= lcs[i * (m + 1) + j + 1])) {
                    all.push_back({'-', std::string(a[head + i++])});
                } else {
                    all.push_back({'+', std::string(b[head + j++])});
                }
            }
        } else {
            for (size_t i = 0; i < n; ++i) {
                all.push_back({'-', std::string(a[head + i])});
            }
            for (size_t j = 0; j < m; ++j) {
                all.push_back({'+', std::string(b[head + j])});
            }
        }
        for (size_t i = a.size() - tail; i < a.size(); ++i) {
            all.push_back({' ', std::string(a[i])});
        }

        // keep the changes and `context` lines around each; a '~' where lines are left out
        std::vector<bool> keep(all.size(), false);
        for (size_t i = 0; i < all.size(); ++i) {
            if (all[i].kind != ' ') {
                const size_t from = i > static_cast<size_t>(context) ? i - context : 0;
                const size_t to = std::min(all.size(), i + context + 1);
                std::fill(keep.begin() + static_cast<long>(from), keep.begin() + static_cast<long>(to), true);
            }
        }
        std::vector<Line> out;
        bool skipped = false;
        for (size_t i = 0; i < all.size(); ++i) {
            if (!keep[i]) {
                skipped = true;
                continue;
            }
            if (skipped && !out.empty()) {
                out.push_back({'~', ""});
            }
            skipped = false;
            out.push_back(std::move(all[i]));
        }
        return out;
    }

} // namespace Diff
