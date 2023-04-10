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
#include <syslog.h>
#include <fcntl.h>
#include <atomic> //to ask if it can be used
#include <dirent.h>

using namespace std;

#define DEFAULT_SLEEP_TIME 20 //in seconds

struct FileInfo {
    string path;
    time_t modification_time{};
    size_t size{};
};

enum Operation {
    DAEMON_SLEEP, //daemon sleep for specified time
    DAEMON_INIT, //initailize daemon (runtime)
    DAEMON_WAKE_UP_BY_SIGNAL, //daemon wake up by signal (SIGUSR1)
    DAEMON_WAKE_UP_BY_TIMER_DEFAULT_TIME,
    DAEMON_WAKE_UP_BY_TIMER_CUSTOM_TIME,
    FILE_REMOVE_SUCCESS,
    FILE_REMOVE_FAILED,
    FILE_COPY_SUCCESS,
    FILE_COPY_FAILED,
    SIGNAL_RECIEVED,
    ERROR
};

//ps aux | grep Demon | grep -v grep | grep -v /bin/bash | awk '{print $2}' | while read pid; do kill -s SIGUSR1 $pid; done
//command to send signal to daemon

namespace settings {
    bool debug = true; //if true - print debug messages
    int sleep_time = 0; //in seconds, if 0 (aditional arg not supplied) then sleep for DEFAULT_SLEEP_TIME
    bool recursive = false; //global variable to check if recursive mode is enabled (only once written, so no need for atomic)

    atomic<bool> recieved_signal(
            false); //used to store if signal was recieved, if true then daemon wake up and reset it to false
    atomic<bool> daemon_busy(false); //used to prevent double daemon wake up (by signal)
}

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
        if (!is_file_or_directory_exists(path)) {
            return false;
        }

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

    size_t get_file_size(const string &path) {
        struct stat file_stat{};
        if (stat(path.c_str(), &file_stat) == -1) {
            return 0;
        }

        return (size_t) file_stat.st_size;
    }

    void scan_files_in_directory(const string &directory, bool recursive, vector<FileInfo> &files) {
        DIR *dir = opendir(directory.c_str());
        if (dir == nullptr) return;
        struct dirent *entry;

        //read all files and directories in current directory
        //if recursive mode is enabled then call this function for each directory
        while ((entry = readdir(dir)) != nullptr) {

            //skip hidden files and directories
            if (string(entry->d_name) == "." || string(entry->d_name) == "..") {
                continue;
            }

            //path with directory name and file name
            string fullPath = directory + "/" + string(entry->d_name);

            //if directory and recursive mode is enabled then call this function for loop directory
            //else add file to files vector
            if (is_a_directory(fullPath) && recursive) {
                scan_files_in_directory(fullPath, recursive, files);
            } else {
                FileInfo file_info;
                file_info.path = fullPath;
                file_info.modification_time = get_file_modification_time(fullPath);
                file_info.size = get_file_size(fullPath);
                files.push_back(file_info);
            }
        }
        closedir(dir);
    }
}

namespace actions {


    string get_operation_name(Operation operation) {
        switch (operation) {
            case Operation::DAEMON_SLEEP:
                return "DAEMON_SLEEP";
            case Operation::DAEMON_INIT:
                return "DAEMON_INIT";
            case Operation::DAEMON_WAKE_UP_BY_SIGNAL:
                return "DAEMON_WAKE_UP_BY_SIGNAL";
            case Operation::DAEMON_WAKE_UP_BY_TIMER_DEFAULT_TIME:
                return "DAEMON_WAKE_UP_BY_TIMER_DEFAULT_TIME";
            case Operation::DAEMON_WAKE_UP_BY_TIMER_CUSTOM_TIME:
                return "DAEMON_WAKE_UP_BY_TIMER_CUSTOM_TIME";
            case Operation::FILE_REMOVE_SUCCESS:
                return "FILE_REMOVE_SUCCESS";
            case Operation::FILE_REMOVE_FAILED:
                return "FILE_REMOVE_FAILED";
            case Operation::FILE_COPY_SUCCESS:
                return "FILE_COPY_SUCCESS";
            case Operation::FILE_COPY_FAILED:
                return "FILE_COPY_FAILED";
            case Operation::SIGNAL_RECIEVED:
                return "SIGNAL_RECIEVED";
            case Operation::ERROR:
                return "ERROR";
        }
        return "UNKNOWN_OPERATION";
    }

    void handle_log(Operation operation, const string &message) {
        //FIXME ask if we have to use our custom date and time function or we can use param for syslog
        string formattedMessage =
                utils::get_current_date_and_time() + " | " + get_operation_name(operation) + " | " + message;
        if (settings::debug) cout << formattedMessage << endl;

        openlog("file_sync_daemon", LOG_PID | LOG_CONS, LOG_USER);
        syslog(LOG_INFO, "%s", formattedMessage.c_str());
        closelog();
    }

    //block thread for specified time until signal is received or time is up
    void handle_daemon_counter() {
        int counter = 0;
        while (counter < settings::sleep_time) {
            if (settings::recieved_signal) {
                settings::recieved_signal = false;
                handle_log(Operation::DAEMON_WAKE_UP_BY_SIGNAL, "Daemon wake up by signal");
                return;
            }
            sleep(1);
            counter++;
        }

        //check if default time is used
        if (settings::sleep_time == DEFAULT_SLEEP_TIME) {
            handle_log(Operation::DAEMON_WAKE_UP_BY_TIMER_DEFAULT_TIME,
                       "Daemon wake up by timer with default time: " + to_string(DEFAULT_SLEEP_TIME) + " seconds");
        } else {
            handle_log(Operation::DAEMON_WAKE_UP_BY_TIMER_CUSTOM_TIME,
                       "Daemon wake up by timer with custom time: " + to_string(settings::sleep_time) + " seconds");
        }
    }

    void handle_aditional_args_parse(const string &arg) {
        if (utils::string_contain(arg, "--sleep_time")) {
            try {
                string sleep_time_str = arg.substr(arg.find('=') + 1);
                settings::sleep_time = stoi(sleep_time_str);

                handle_log(Operation::DAEMON_INIT,
                           "Daemon initialized with custom sleep time: " + to_string(settings::sleep_time) +
                           " seconds");
            } catch (exception &e) {
                cerr << "Failed to parse sleep time parametr " << arg << " due to: " << e.what() << endl;
                exit(-1);
            }
        }

        if (arg == "-R") {
            settings::recursive = true;
            handle_log(Operation::DAEMON_INIT, "Daemon initialized with recursive mode enabled");
        }
    }

    bool handle_input_directories_validation(const string &sourcePath, const string &destinationPath) {
        if (!utils::is_file_or_directory_exists(sourcePath)) {
            cerr << "Source path " << sourcePath << " does not exist" << endl;
            return false;
        }

        if (!utils::is_file_or_directory_exists(destinationPath)) {
            cerr << "Destination path " << destinationPath << " does not exist" << endl;
            return false;
        }

        if (!utils::is_a_directory(sourcePath)) {
            cerr << "Source path " << sourcePath << " is not a directory" << endl;
            return false;
        }

        if (!utils::is_a_directory(destinationPath)) {
            cerr << "Destination path " << destinationPath << " is not a directory" << endl;
            return false;
        }

        return true;
    }
}

namespace handlers {
    void signal_handler(int signum) {
        if (signum != SIGUSR1) return;
        //check if daemon is busy, if so, ignore signal
        if (settings::daemon_busy) {
            actions::handle_log(Operation::SIGNAL_RECIEVED, "Signal USR1 received, but daemon is busy");
            return;
        }

        //set settings recieved signal to true, so daemon can wake up
        actions::handle_log(Operation::SIGNAL_RECIEVED, "Signal USR1 received");
        settings::recieved_signal = true;
    }

    [[noreturn]] void daemon_handler(const string &sourcePath, const string &destinationPath) {
        while (true) {

            //block current thread until signal is received or sleep time is up
            //daemon wake up logging is handled in handle_daemon_counter
            actions::handle_daemon_counter();

            settings::daemon_busy = true;
            vector<FileInfo> sourceDirFiles = {};
            vector<FileInfo> destinationDirFiles = {};

            utils::scan_files_in_directory(sourcePath, settings::recursive, sourceDirFiles);

            //check if source directory is empty, if so, skip this iteration
            if (sourceDirFiles.empty()) {
                actions::handle_log(Operation::DAEMON_SLEEP, "No files found in source directory");
                settings::daemon_busy = false;
                continue;
            }

            utils::scan_files_in_directory(destinationPath, settings::recursive, destinationDirFiles);
            cout << "Found " << sourceDirFiles.size() << " sourceDirFiles in source directory" << endl;

            settings::daemon_busy = false;
            actions::handle_log(Operation::DAEMON_SLEEP, "Daemon finished, counter reset");
        }
    }
}

int main(int argc, char *argv[]) {

    if (argc < 3) {
        cerr << "Not enough arguments supplied, expected 3, got " << argc << endl;
        cerr << "Usage: " << argv[0] << " <source_path> <destination_path> <aditional_args>" << endl;
        return -1;
    }

    string sourcePath = argv[1];
    string destinationPath = argv[2];

    //verify input directories
    if (!actions::handle_input_directories_validation(sourcePath, destinationPath)) {
        return -1;
    }

    actions::handle_log(Operation::DAEMON_INIT,
                        "Daemon initialized with source path: " + sourcePath + " and destination path: " +
                        destinationPath);

    //<editor-fold desc="aditional args parse">
    vector<string> aditionalArgs;
    for (int i = 3; i < argc; i++) {
        aditionalArgs.emplace_back(argv[i]);
    }

    for (const auto &item: aditionalArgs) {
        actions::handle_aditional_args_parse(item);
    }

    //fixup sleep time if aditional arg was not supplied
    if (settings::sleep_time == 0) {
        settings::sleep_time = DEFAULT_SLEEP_TIME;
    }
    //</editor-fold>

    signal(SIGUSR1, handlers::signal_handler);

    handlers::daemon_handler(sourcePath, destinationPath);
    return 0;
}