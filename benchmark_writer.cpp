#include "benchmark_writer.h"

#include <fstream>
#include <iomanip>
#include <stdexcept>

struct BenchmarkWriter::Impl
{
    std::ofstream out;
};

BenchmarkWriter::BenchmarkWriter(const std::string &out_path)
    : impl(new Impl())
{
    impl->out.open(out_path);
    if (!impl->out.is_open())
    {
        delete impl;
        throw std::ios_base::failure("BenchmarkWriter: failed to open " + out_path);
    }
    impl->out << "algorithm,n,time_seconds,result_digits\n";
    impl->out.flush();
}

BenchmarkWriter::~BenchmarkWriter()
{
    close();
    delete impl;
}

void BenchmarkWriter::write_sample(const std::string &algorithm, const std::string &n,
                                    double time_seconds, std::size_t result_digits)
{
    impl->out << algorithm << "," << n << "," << std::setprecision(9) << time_seconds
               << "," << result_digits << "\n";
    impl->out.flush(); // every sample is persisted immediately -- see header comment
}

void BenchmarkWriter::close()
{
    if (impl->out.is_open())
    {
        impl->out.close();
    }
}
