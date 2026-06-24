#include "csv_reader.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ui
{

static std::vector<std::string> split_line(const std::string &line)
{
    std::vector<std::string> fields;
    std::stringstream ss(line);
    std::string field;
    while (std::getline(ss, field, ','))
    {
        fields.push_back(field);
    }
    return fields;
}

BenchmarkData load_csv(const std::string &path)
{
    BenchmarkData data;
    data.source_path = path;

    std::ifstream f(path);
    if (!f.is_open())
    {
        data.error = "Could not open file: " + path;
        return data;
    }

    // stable insertion-order map: algo name -> index into data.series
    std::unordered_map<std::string, size_t> index;

    std::string line;
    bool first = true;
    while (std::getline(f, line))
    {
        // strip trailing \r (Windows line endings)
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }

        // skip header
        if (first)
        {
            first = false;
            continue;
        }

        if (line.empty()) continue;

        auto fields = split_line(line);
        if (fields.size() < 4) continue; // malformed, skip silently

        try
        {
            std::string algo         = fields[0];
            double n                 = std::stod(fields[1]);
            double time_seconds      = std::stod(fields[2]);
            size_t result_digits     = std::stoull(fields[3]);

            // find or create series for this algo
            auto it = index.find(algo);
            if (it == index.end())
            {
                index[algo] = data.series.size();
                data.series.push_back({ algo, {} });
                it = index.find(algo);
            }

            data.series[it->second].samples.push_back({ n, time_seconds, result_digits });
        }
        catch (...)
        {
            continue; // malformed field, skip row
        }
    }

    if (data.series.empty())
    {
        data.error = "No valid data rows found in: " + path;
    }

    return data;
}

} // namespace ui
