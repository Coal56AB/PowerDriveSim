#include "formats/project/project.hpp"
#include "results/csv.hpp"
#include <fstream>
#include <iostream>
int main(int argc,char** argv) {
    if(argc<2 || argc>3) { std::cerr << "Usage: powerdrive-cli project.pds [output.csv]\n"; return 2; }
    try {
        std::ifstream input(argv[1]);
        if(!input) throw pds::Diagnostic("read_error",argv[1],"Cannot open project");
        auto project=pds::read_project(input);
        auto result=pds::execute(pds::compile(project));
        if(argc==3) { std::ofstream out(argv[2]); pds::write_csv(result,out); }
        std::cout << "Reference CPU | float64 | Backward Euler | steps=" << result.accepted_steps
                  << " | samples=" << result.samples.size() << " | residual=" << result.max_scaled_residual << '\n';
        return 0;
    } catch(const pds::Diagnostic& e) {
        std::cerr << e.code << " object=" << e.object << " time=" << e.time << ": " << e.what() << '\n'; return 1;
    } catch(const std::exception& e) { std::cerr << "error: " << e.what() << '\n'; return 1; }
}