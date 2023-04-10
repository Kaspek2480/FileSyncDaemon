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
#include <sys/mman.h>
#include <utime.h>
#include <syslog.h>
#include <fcntl.h>
#include <atomic> //to ask if it can be used
#include <dirent.h>

using namespace std;

#define DEFAULT_SLEEP_TIME 20 //in seconds

struct FileInfo {
    string path;

    //path to file in mirror directory, like /home/user/archive/1/2/file.txt -> /home/user/backup/1/2/file.txt
    //or /home/user/backup/1/2/file.txt -> /home/user/archive/1/2/file.txt
    string mirrorPath;
    time_t lastModified{};
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
    DAEMON_INIT_ERROR,
    DAEMON_WORK_INFO
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
    string get_operation_name(Operation operation) {
        switch (operation) {
            case DAEMON_SLEEP:
                return "DAEMON_SLEEP";
            case DAEMON_INIT:
                return "DAEMON_INIT";
            case DAEMON_WAKE_UP_BY_SIGNAL:
                return "DAEMON_WAKE_UP_BY_SIGNAL";
            case DAEMON_WAKE_UP_BY_TIMER_DEFAULT_TIME:
                return "DAEMON_WAKE_UP_BY_TIMER_DEFAULT_TIME";
            case DAEMON_WAKE_UP_BY_TIMER_CUSTOM_TIME:
                return "DAEMON_WAKE_UP_BY_TIMER_CUSTOM_TIME";
            case FILE_REMOVE_SUCCESS:
                return "FILE_REMOVE_SUCCESS";
            case FILE_REMOVE_FAILED:
                return "FILE_REMOVE_FAILED";
            case FILE_COPY_SUCCESS:
                return "FILE_COPY_SUCCESS";
            case FILE_COPY_FAILED:
                return "FILE_COPY_FAILED";
            case SIGNAL_RECIEVED:
                return "SIGNAL_RECIEVED";
            case DAEMON_INIT_ERROR:
                return "DAEMON_INIT_ERROR";
            case DAEMON_WORK_INFO:
                return "DAEMON_WORK_INFO";
        }
        return "UNKNOWN_OPERATION";
    }

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

    //save logs to syslog
    void log(Operation operation, const string &message) {
        //FIXME ask if we have to use our custom date and time function or we can use param for syslog
        string formattedMessage =
                get_current_date_and_time() + " | " + get_operation_name(operation) + " | " + message;
        if (settings::debug) cout << formattedMessage << endl;

        openlog("file_sync_daemon", LOG_PID | LOG_CONS, LOG_USER);
        syslog(LOG_INFO, "%s", formattedMessage.c_str());
        closelog();
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

    bool read_write_file_copy(const string &source, const string &destination) {
        //use linux read/write system calls
        int sourceFd = open(source.c_str(), O_RDONLY);
        int destinationFd = open(destination.c_str(), O_WRONLY | O_CREAT, 0666);
        if (sourceFd == -1 || destinationFd == -1) {
            return false;
        }

        char buffer[1024];
        ssize_t readBytes;
        while ((readBytes = read(sourceFd, buffer, sizeof(buffer))) > 0) {
            if (write(destinationFd, buffer, readBytes) != readBytes) {
                return false;
            }
        }
        close(sourceFd);
        close(destinationFd);
        return true;
    }

    bool mmap_file_copy(const FileInfo &source, const string &destination) {
        int sourceFd = open(source.path.c_str(), O_RDONLY);
        int destinationFd = open(destination.c_str(), O_WRONLY | O_CREAT, 0666);
        if (sourceFd == -1 || destinationFd == -1) {
            return false;
        }

        //map source file to memory
        char *sourceMap = (char *) mmap(nullptr, source.size, PROT_READ, MAP_PRIVATE, sourceFd, 0);
        if (sourceMap == MAP_FAILED) {
            return false;
        }

        //write source file to destination path
        if (write(destinationFd, sourceMap, source.size) != source.size) {
            return false;
        }

        //deallocating map memory
        munmap(sourceMap, source.size);

        close(sourceFd);
        close(destinationFd);
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

    bool create_subdirectories(const string &path) {
        string path_copy = path;
        string path_to_create;

        while (path_copy.find('/') != string::npos) {

            //appending next directory to path, one by one
            path_to_create += path_copy.substr(0, path_copy.find('/') + 1);

            //removing already added directory from path
            path_copy = path_copy.substr(path_copy.find('/') + 1);

            //if directory already exists, then skip it
            if (utils::is_a_directory(path_to_create)) {
                continue;
            }

            //try to create directory
            //if failed - we can't continue
            if (!utils::directory_create(path_to_create)) {
                return false;
            }
        }
        return true;
    }

    size_t get_file_size(const string &path) {
        struct stat file_stat{};
        if (stat(path.c_str(), &file_stat) == -1) {
            return 0;
        }

        return (size_t) file_stat.st_size;
    }

    bool file_copy(const FileInfo &source, const string &destination) {
        //check if size is bigger than 5MB
        bool result = false;
        if (source.size > 5 * 1024 * 1024) {
            //if yes, then use mmap
            result = mmap_file_copy(source, destination);
        } else {
            //if no, then use normal file copy
            result = read_write_file_copy(source.path, destination);
        }

        //if copy was successful, then change modification time
        if (result) {
            change_file_modification_time(destination, source.lastModified);
        }

        //log result
        if (result) {
            log(Operation::FILE_COPY_FAILED, "Failed to copy file " + source.path + " to " + destination);
        } else {
            log(Operation::FILE_COPY_SUCCESS, "Successfully copied file " + source.path + " to " + destination);
        }

        return result;
    }

    //mirroredPath is inverse path to directory where files are synced
    //work like mirror, for example:

    //directory: /home/user/archive
    //mirroredPath: /home/user/backup

    //filePath: /home/user/archive/1/2/file.txt
    //mirroredPath: /home/user/backup/1/2/file.txt

    //recursivePathCollector is used to store path to directory where files are stored (help variable)
    void scan_files_in_directory(const string &directory, bool recursive,
                                 vector<FileInfo> &files, const string &mirroredPath, string &recursivePathCollector) {
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

            //if file is not directory then add it to files vector
            if (!is_a_directory(fullPath)) {
                FileInfo file_info;
                file_info.path = fullPath;
                file_info.mirrorPath = mirroredPath + "/" + recursivePathCollector + string(entry->d_name);
                file_info.lastModified = get_file_modification_time(fullPath);
                file_info.size = get_file_size(fullPath);
                files.push_back(file_info);
                continue;
            }

            //if recursive mode is disabled then skip directory
            if (!recursive) {
                continue;
            }

            //we are in recursive call and current file is directory
            //so add directory name to recursivePathCollector and call this function recursively for this directory
            recursivePathCollector += string(entry->d_name) + "/";

            scan_files_in_directory(fullPath, recursive, files, mirroredPath, recursivePathCollector);

            //exiting from recursive call, so remove last directory name from recursivePathCollector with `/` at the end
            recursivePathCollector = recursivePathCollector.substr(0, recursivePathCollector.size() -
                                                                      string(entry->d_name).size() - 1);
        }

        closedir(dir);
    }

    bool transform_to_daemon() {
        pid_t pid = fork();

        //failed to fork so deamon can't be created
        if (pid < 0) {
            return false;
        }

        //kill parent process
        if (pid > 0) {
            exit(EXIT_SUCCESS);
        }

        //create new session and process group
        if (setsid() < 0) {
            return false;
        }

        //ignore signals from terminal, we don't need them
        signal(SIGCHLD, SIG_IGN);
        signal(SIGHUP, SIG_IGN);

        pid = fork();
        if (pid < 0) {
            return false;
        }
        if (pid > 0) {
            exit(EXIT_SUCCESS);
        }

        //set new file permissions
        if (umask(0) == -1) {
            return false;
        }

        //set working directory to root
        if (chdir("/") == -1) {
            return false;
        }

        //close stdin, stdout and stderr
        if (close(STDIN_FILENO) == -1) {
            return false;
        }
        if (close(STDOUT_FILENO) == -1) {
            return false;
        }
        if (close(STDERR_FILENO) == -1) {
            return false;
        }

        //close all open file descriptors without stdin, stdout and stderr
        //stdin - 0, stdout - 1, stderr - 2
        for (int i = 3; i < sysconf(_SC_OPEN_MAX); i++) {
            close(i);
        }

        //open stdin, stdout and stderr to /dev/null
        int fd = open("/dev/null", O_RDWR);
        if (fd == -1) {
            return false;
        }

        if (dup2(fd, STDIN_FILENO) == -1) {
            return false;
        }
        if (dup2(fd, STDOUT_FILENO) == -1) {
            return false;
        }
        if (dup2(fd, STDERR_FILENO) == -1) {
            return false;
        }

        //TODO ask if daemon() function can be used instead of this function
        return true;
    }
}

namespace actions {

    //block thread for specified time until signal is received or time is up
    void handle_daemon_counter() {
        int counter = 0;
        while (counter < settings::sleep_time) {
            if (settings::recieved_signal) {
                settings::recieved_signal = false;
                utils::log(Operation::DAEMON_WAKE_UP_BY_SIGNAL, "Daemon wake up by signal");
                return;
            }
            sleep(1);
            counter++;
        }

        //check if default time is used
        if (settings::sleep_time == DEFAULT_SLEEP_TIME) {
            utils::log(Operation::DAEMON_WAKE_UP_BY_TIMER_DEFAULT_TIME,
                       "Daemon wake up by timer with default time: " + to_string(DEFAULT_SLEEP_TIME) +
                       " seconds");
        } else {
            utils::log(Operation::DAEMON_WAKE_UP_BY_TIMER_CUSTOM_TIME,
                       "Daemon wake up by timer with custom time: " + to_string(settings::sleep_time) +
                       " seconds");
        }
    }

    void handle_aditional_args_parse(const string &arg) {
        if (utils::string_contain(arg, "--sleep_time")) {
            try {
                string sleep_time_str = arg.substr(arg.find('=') + 1);
                settings::sleep_time = stoi(sleep_time_str);

                utils::log(Operation::DAEMON_INIT,
                           "Custom sleep time: " + to_string(settings::sleep_time) +
                           " seconds");
            } catch (exception &e) {
                cerr << "Failed to parse sleep time parametr " << arg << " due to: " << e.what() << endl;
                utils::log(Operation::DAEMON_INIT_ERROR,
                           "Failed to parse sleep time parametr " + arg + " due to: " + e.what());
                exit(-1);
            }
        }

        if (arg == "-R") {
            settings::recursive = true;
            utils::log(Operation::DAEMON_INIT, "Recursive mode enabled");
        }
    }

    bool validate_input_dirs(const string &sourcePath, const string &destinationPath) {
        if (!utils::is_file_or_directory_exists(sourcePath)) {
            cerr << "Source path " << sourcePath << " does not exist" << endl;
            utils::log(Operation::DAEMON_INIT_ERROR, "Source path " + sourcePath + " does not exist");
            return false;
        }

        if (!utils::is_file_or_directory_exists(destinationPath)) {
            cerr << "Destination path " << destinationPath << " does not exist" << endl;
            utils::log(Operation::DAEMON_INIT_ERROR, "Destination path " + destinationPath + " does not exist");
            return false;
        }

        if (!utils::is_a_directory(sourcePath)) {
            cerr << "Source path " << sourcePath << " is not a directory" << endl;
            utils::log(Operation::DAEMON_INIT_ERROR, "Source path " + sourcePath + " is not a directory");
            return false;
        }

        if (!utils::is_a_directory(destinationPath)) {
            cerr << "Destination path " << destinationPath << " is not a directory" << endl;
            utils::log(Operation::DAEMON_INIT_ERROR,
                       "Destination path " + destinationPath + " is not a directory");
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
            utils::log(Operation::SIGNAL_RECIEVED, "Signal USR1 received, but daemon is busy");
            return;
        }

        //set settings recieved signal to true, so daemon can wake up
        utils::log(Operation::SIGNAL_RECIEVED, "Signal USR1 received");
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

            string recursiveHelp;
            utils::scan_files_in_directory(sourcePath, settings::recursive, sourceDirFiles, destinationPath,
                                           recursiveHelp);

            //check if source directory is empty, if so, skip this iteration
            if (sourceDirFiles.empty()) {
                utils::log(Operation::DAEMON_SLEEP, "No files found in source directory");
                settings::daemon_busy = false;
                continue;
            }

            utils::scan_files_in_directory(destinationPath, settings::recursive, destinationDirFiles, sourcePath,
                                           recursiveHelp);
            utils::log(Operation::DAEMON_WORK_INFO, "Scanning directories finished, found " +
                                                    to_string(sourceDirFiles.size()) +
                                                    " files in source directory and " +
                                                    to_string(destinationDirFiles.size()) +
                                                    " files in destination directory");
            for (const auto &item: sourceDirFiles) {
                cout << "Full path: " << item.path << "\nMirrored path: " << item.mirrorPath << "\nsize: " << item.size
                     << "\nlast modified: " << item.lastModified << endl << endl;
            }

            //check if destination directory is empty, if so, copy all files from source directory
            if (destinationDirFiles.empty()) {
                utils::log(Operation::DAEMON_WORK_INFO,
                           "Destination directory is empty, copying all files from source directory");
                for (const auto &file: sourceDirFiles) {
                    utils::file_copy(file, file.mirrorPath);
                }

                settings::daemon_busy = false;
                utils::log(Operation::DAEMON_SLEEP, "Daemon finished work, counter reset");
                continue;
            }

            //check if files in source directory are already in destination directory
            //if so, check if they are the same, if not, copy them
            for (const auto &file: sourceDirFiles) {
                bool found = false;
                for (const auto &mirrorFile: destinationDirFiles) {
                    if (file.path == mirrorFile.path) {
                        found = true;

                        //check if files are the same
                        if (file.size != mirrorFile.size || file.lastModified != mirrorFile.lastModified) {
                            utils::file_copy(file, file.mirrorPath);
                        }
                    }
                }

                //file not found in destination directory, copy it
                if (!found) {
                    utils::file_copy(file, file.mirrorPath);
                }
            }

            //check if files in destination directory are not in source directory
            //if so, delete them
            for (const auto &file: destinationDirFiles) {
                bool found = false;
                for (const auto &mirrorFile: sourceDirFiles) {
                    if (file.path == mirrorFile.path) {
                        found = true;
                    }
                }

                //file not found in source directory, delete it
                if (!found) {
                    if (!utils::file_delete(file.path)) {
                        utils::log(Operation::FILE_REMOVE_FAILED,
                                   "Error deleting file " + file.path + " from destination directory");
                    } else {
                        utils::log(Operation::FILE_REMOVE_SUCCESS,
                                   "File " + file.path + " deleted, not found in source directory");
                    }
                }
            }

            //reset daemon busy flag
            settings::daemon_busy = false;
            utils::log(Operation::DAEMON_SLEEP, "Daemon finished, counter reset");
            exit(0);
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
    if (!actions::validate_input_dirs(sourcePath, destinationPath)) {
        return -1;
    }

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

    utils::log(Operation::DAEMON_INIT,
               "Daemon initialized with source path: " + sourcePath + " and destination path: " +
               destinationPath + " and sleep time: " + to_string(settings::sleep_time) + " seconds");

    signal(SIGUSR1, handlers::signal_handler);

    handlers::daemon_handler(sourcePath, destinationPath);
}