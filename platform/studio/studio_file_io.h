#pragma once
#include <string>
#include <vector>
namespace studio {
// Write a complete bounded output beside its destination, then atomically replace.
bool atomic_write_file(const std::string &path,const std::vector<unsigned char> &bytes,std::string &error);
}
