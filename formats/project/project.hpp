#pragma once
#include "core/model/model.hpp"
#include <iosfwd>
namespace pds {
Project read_project(std::istream& stream);
void write_project(const Project& project, std::ostream& stream);
}