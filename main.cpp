#include <iostream>
#include <filesystem>
#include <iomanip>
#include <ctime>
#include <fstream>
#include <cstdio>
#include <unistd.h>
#include <csignal>
#include <vector>
#include <sys/stat.h>
#include <utime.h>
#include <fcntl.h>
#include <atomic> //to ask if it can be used

using namespace std;

#define DEFAULT_SLEEP_TIME 300 //in seconds
bool debug = true; //if true - print debug messages

int sleep_time = 0; //in seconds, if 0 (aditional arg not supplied) then sleep for DEFAULT_SLEEP_TIME
bool recursive = false; //global variable to check if recursive mode is enabled (only once written, so no need for atomic)

atomic<bool> recieved_signal(false);
atomic<bool> daemon_currently_working(false); //used to prevent double daemon wake up (by signal)

enum Operation : int {
    DAEMON_SLEEP = 0, //daemon sleep for specified time
    DAEMON_INIT = 1, //initailize daemon (runtime)
    DAEMON_WAKE_UP_BY_SIGNAL = 2, //daemon wake up by signal (SIGUSR1)
    DAEMON_WAKE_UP_BY_TIMER_DEFAULT_TIME = 3,
    DAEMON_WAKE_UP_BY_TIMER_CUSTOM_TIME = 3,
    FILE_REMOVE_SUCCESS = 2,
    FILE_REMOVE_FAILED = 3,
    FILE_COPY_SUCCESS = 4,
    FILE_COPY_FAILED = 5,
};

namespace utils {
    bool string_contain(const string &text, const string &contains) {
        if (text.find(contains, 0) != string::npos) {
            return true;
        }
        return false;
    }

    string get_current_date_and_time() {
        ostringstream oss;
        time_t now = time(nullptr);
        tm *local_time = localtime(&now);
        oss << put_time(local_time, "%Y-%m-%d %H:%M:%S");
        return oss.str();
    }

    bool is_file_or_directory_exists(const string &path) {
        if (access(path.c_str(), F_OK) != -1) {
            return true;
        }
        return false;
    }

    bool is_a_directory(const string &path) {
        struct stat path_stat{};
        stat(path.c_str(), &path_stat);
        return S_ISDIR(path_stat.st_mode);
    }

    time_t get_file_modification_time(const string &path) {
        struct stat path_stat{};
        stat(path.c_str(), &path_stat);
        return path_stat.st_mtime;
    }

    bool change_file_modification_time(const string &path, time_t time) {
        struct utimbuf new_times{};
        new_times.actime = time;
        new_times.modtime = time;
        if (utime(path.c_str(), &new_times) == 0) {
            return true;
        }
        return false;
    }

    bool normal_file_copy(const string &source, const string &destination) {
        //use linux read/write system calls
        int source_fd = open(source.c_str(), O_RDONLY);
        int destination_fd = open(destination.c_str(), O_WRONLY | O_CREAT, 0666);
        if (source_fd == -1 || destination_fd == -1) {
            return false;
        }

        char buffer[1024];
        ssize_t read_bytes;
        while ((read_bytes = read(source_fd, buffer, sizeof(buffer))) > 0) {
            if (write(destination_fd, buffer, read_bytes) != read_bytes) {
                return false;
            }
        }
        close(source_fd);
        close(destination_fd);
        return true;
    }

    bool file_delete(const string &path) {
        if (remove(path.c_str()) == 0) {
            return true;
        }
        return false;
    }

    bool directory_delete(const string &path) {
        if (rmdir(path.c_str()) == 0) {
            return true;
        }
        return false;
    }

    bool directory_create(const string &path) {
        if (mkdir(path.c_str(), 0777) == 0) {
            return true;
        }
        return false;
    }
}

namespace actions {
    void handle_log(Operation operation, const string &formatted_message) {
        //log to syslog
    }

    //block thread for specified time until signal is received or time is up
    void handle_daemon_counter() {

    }

    void parse_aditional_args(const string &arg) {
        if (utils::string_contain(arg, "--sleep_time")) {
            try {
                string sleep_time_str = arg.substr(arg.find('=') + 1);
                if (debug) cout << "Sleep time parametr present with value: " << sleep_time_str << endl;
                sleep_time = stoi(sleep_time_str);
            } catch (exception &e) {
                cerr << "Failed to parse sleep time parametr " << arg << " due to: " << e.what() << endl;
                exit(-1);
            }
        }

        if (arg == "-R") {
            string max_sleep_time = arg.substr(arg.find('=') + 1);
            if (debug) cout << "R parametr present, recursive mode enabled" << endl;
            //TODO log to syslog that recursive mode is enabled
            recursive = true;
        }
    }

    bool verify_input_directories(const string &source_path, const string &destination_path) {
        if (!utils::is_file_or_directory_exists(source_path)) {
            cerr << "Source path " << source_path << " does not exist" << endl;
            return false;
        }

        if (!utils::is_file_or_directory_exists(destination_path)) {
            cerr << "Destination path " << destination_path << " does not exist" << endl;
            return false;
        }

        if (!utils::is_a_directory(source_path)) {
            cerr << "Source path " << source_path << " is not a directory" << endl;
            return false;
        }

        if (!utils::is_a_directory(destination_path)) {
            cerr << "Destination path " << destination_path << " is not a directory" << endl;
            return false;
        }

        return true;
    }
}

namespace handlers {
    void signal_handler(int signum) {
        if (debug) cout << "Signal " << signum << " received" << endl;
        if (signum == SIGUSR1) {
            //wake up daemon

            //TODO log to syslog that daemon received signal to wake up

            recieved_signal = true;
        }
    }

    void daemon_handler(const string &source_path, const string &destination_path) {
        daemon_currently_working = true;
        if (debug) cout << "Daemon started" << endl;

        //TODO log to syslog that daemon started

        daemon_currently_working = false;
    }
}

int main(int argc, char *argv[]) {

    if (argc <= 3) {
        cerr << "Not enough arguments supplied, expected 3, got " << argc << endl;
        cerr << "Usage: " << argv[0] << " <source_path> <destination_path> <aditional_args>" << endl;
        return -1;
    }

    string sourcePath = argv[1];
    string destinationPath = argv[2];

    //verify input directories
    if (!actions::verify_input_directories(sourcePath, destinationPath)) {
        return -1;
    }

    //<editor-fold desc="aditional args parse">
    vector<string> aditionalArgs;
    for (int i = 3; i < argc; i++) {
        aditionalArgs.emplace_back(argv[i]);
    }

    for (const auto &item: aditionalArgs) {
        if (debug) cout << "Aditional args: " << item << endl;
        actions::parse_aditional_args(item);
    }

    //fixup sleep time if aditional arg was not supplied
    if (sleep_time == 0) {
        sleep_time = DEFAULT_SLEEP_TIME;
    }
    //</editor-fold>

    signal(SIGUSR1, handlers::signal_handler);

    while (true) {
        sleep(1);
    }

    return 0;
}