#include "formats/samples/table.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
namespace pds {
namespace {
std::string trim(std::string s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return {};
    return s.substr(begin, s.find_last_not_of(" \t\r\n") - begin + 1);
}
std::vector<std::string> cells(const std::string &line, char delimiter) {
    if (!delimiter) {
        std::istringstream input(line);
        std::vector<std::string> result;
        for (std::string token; input >> token;)
            result.push_back(token);
        return result;
    }
    std::vector<std::string> result;
    std::string cell;
    bool quoted = false, closed = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (quoted) {
            if (c != '"')
                cell += c;
            else if (i + 1 < line.size() && line[i + 1] == '"') {
                cell += c;
                ++i;
            } else {
                quoted = false;
                closed = true;
            }
        } else if (c == delimiter) {
            result.push_back(trim(cell));
            cell.clear();
            closed = false;
        } else if (c == '"' && !closed && trim(cell).empty()) {
            cell.clear();
            quoted = true;
        } else if (c == '"' || (closed && !std::isspace(static_cast<unsigned char>(c))))
            throw std::runtime_error("Malformed quoted field");
        else
            cell += c;
    }
    if (quoted)
        throw std::runtime_error("Unclosed quoted field");
    result.push_back(trim(cell));
    return result;
}
bool time_header(std::string label) {
    const auto bracket = label.find('[');
    if (bracket != std::string::npos)
        label = trim(label.substr(0, bracket));
    std::transform(label.begin(), label.end(), label.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return label == "time" || label == "t" || label == "time_seconds";
}
double header_scale(const std::string &label, const std::string &unit) {
    const auto begin = label.find('['), end = label.find(']');
    if (begin == std::string::npos && end == std::string::npos)
        return 1;
    if (begin == std::string::npos || end == std::string::npos || end <= begin + 1)
        throw std::runtime_error("Malformed unit in column header");
    return parse_si("1 " + label.substr(begin + 1, end - begin - 1), unit);
}
double number(std::string cell, const std::string &unit, double scale, bool decimal_comma) {
    if (decimal_comma)
        std::replace(cell.begin(), cell.end(), ',', '.');
    // A suffix in a cell carries its own unit; header units apply to bare numbers only.
    const auto last = cell.find_last_not_of(" \t\r");
    const bool suffix = last != std::string::npos && !std::isdigit(static_cast<unsigned char>(cell[last])) &&
                        cell[last] != '.';
    const double result = parse_si(cell, unit) * (suffix ? 1 : scale);
    if (!std::isfinite(result))
        throw std::runtime_error("Value is outside the supported range");
    return result;
}
} // namespace
std::vector<Point> read_sample_table(std::istream &input, const std::string &value_unit) {
    std::vector<Point> result;
    size_t line_number = 0, bytes = 0;
    bool first = true;
    char delimiter = 0;
    double time_scale = 1, value_scale = 1;
    try {
        for (std::string line; std::getline(input, line);) {
            ++line_number;
            bytes += line.size() + 1;
            if (bytes > sample_table_max_bytes)
                throw std::runtime_error("Table exceeds 16 MiB");
            if (line_number == 1 && line.rfind("\xef\xbb\xbf", 0) == 0)
                line.erase(0, 3);
            line = trim(line);
            if (line.empty() || line[0] == '#')
                continue;
            if (first) {
                // Semicolon/tab tables allow decimal commas; comma CSV uses decimal dots.
                std::string separators;
                bool quoted = false;
                for (char c : line) {
                    if (c == '"')
                        quoted = !quoted;
                    else if (!quoted && (c == ';' || c == '\t' || c == ','))
                        separators += c;
                }
                for (char candidate : {';', '\t', ','})
                    if (separators.find(candidate) != std::string::npos) {
                        delimiter = candidate;
                        break;
                    }
            }
            auto row = cells(line, delimiter);
            if (row.size() != 2)
                throw std::runtime_error("Expected exactly two columns: time and value");
            if (first && time_header(row[0])) {
                time_scale = header_scale(row[0], "s");
                value_scale = header_scale(row[1], value_unit);
                first = false;
                continue;
            }
            first = false;
            Point point{number(row[0], "s", time_scale, delimiter != ','),
                        number(row[1], value_unit, value_scale, delimiter != ',')};
            if (point.x < 0 || (!result.empty() && point.x <= result.back().x))
                throw std::runtime_error("Time must be nonnegative and strictly increasing");
            if (result.size() == sample_table_max_rows)
                throw std::runtime_error("Table exceeds 100000 rows");
            result.push_back(point);
        }
        if (input.bad())
            throw std::runtime_error("Could not read table");
        if (result.empty())
            throw std::runtime_error("Table has no samples");
    } catch (const std::exception &e) {
        throw Diagnostic("invalid_samples", "", "Line " + std::to_string(line_number) + ": " + e.what());
    }
    return result;
}
} // namespace pds
