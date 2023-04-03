#include <iostream>
#include <filesystem>
#include <iomanip>
#include <ctime>


using namespace std;

int sleepTime = 300;
bool recursive = false;
bool debug = false;

namespace utils {
    bool string_contain(const string &text, const string &contains) {
        if (text.find(contains, 0) != string::npos) {
            return true;
        }
        return false;
    }
}

void parse_aditional_args(const string &arg) {
    if (utils::string_contain(arg, "--sleep_time")) {
        try {
            string sleep_time = arg.substr(arg.find('=') + 1);
            if (debug) cout << "Sleep time parametr present with value: " << sleep_time << endl;
            sleepTime = stoi(sleep_time);
        } catch (exception &e) {
            cerr << "Failed to parse sleep time parametr " << arg << " due to: " << e.what() << endl;
            exit(-1);
        }
    }

    if (arg == "-R") {
        string max_sleep_time = arg.substr(arg.find('=') + 1);
        if (debug) cout << "R parametr present" << endl;
        recursive = true;
    }
}

string get_date_and_time() {
    ostringstream oss;
    time_t now = time(nullptr);
    tm *local_time = localtime(&now);
    oss << put_time(local_time, "%Y-%m-%d %H-%M:%S");
    return oss.str();
}

int main(int argc, char *argv[]) {

    if (argc <= 3) {
        cerr << "Not enough arguments supplied, expected 3, got " << argc << endl;
        cerr << "Usage: " << argv[0] << " <source_path> <destination_path> <aditional_args>" << endl;
        return -1;
    }

    //<editor-fold desc="source path">
    string sourcePath = argv[1];

    if (filesystem::exists(sourcePath) == 0) {
        cerr << "Source directory does not exist" << endl;
        return -1;
    }
    if (filesystem::is_directory(sourcePath) == 0) {
        cerr << "Source path is not a directory" << endl;
        return -1;
    }
    //</editor-fold>

    //<editor-fold desc="destination path">
    string destinationPath = argv[2];

    if (filesystem::exists(destinationPath) == 0) {
        cout << "Destination directory does not exist" << endl;
        return -1;
    }
    if (filesystem::is_directory(destinationPath) == 0) {
        cerr << "Destination path is not a directory" << endl;
        return -1;
    }
    //</editor-fold>

    //<editor-fold desc="aditional args parse">
    vector <string> aditionalArgs;
    for (int i = 3; i < argc; i++) {
        aditionalArgs.emplace_back(argv[i]);
    }

    for (const auto &item: aditionalArgs) {
        if (debug) cout << "Aditional args: " << item << endl;
    }

    for (const auto &item: aditionalArgs) {
        parse_aditional_args(item);
    }
    //</editor-fold>

    return 0;
}