#include <iostream>
#include <filesystem>
#include <iomanip>
#include <ctime>
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
#include <cstring>

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
    DAEMON_INIT, //initialize daemon (runtime)
    DAEMON_WAKE_UP_BY_SIGNAL, //daemon wake up by signal (SIGUSR1)
    DAEMON_WAKE_UP_DEFAULT_TIMER,
    DAEMON_WAKE_UP_CUSTOM_TIMER,
    SIGNAL_received,
    DAEMON_INIT_ERROR,
    DAEMON_WORK_INFO,
    FILE_OPERATION_INFO,
    FILE_OPERATION_ERROR,
};

//ps aux | grep Demon | grep -v grep | grep -v /bin/bash | awk '{print $2}' | while read pid; do kill -s SIGUSR1 $pid; done
//command to send signal to daemon

//head -c 5MB /dev/zero > ostechnix.txt
//command to create 5MB file

//in WSL we must enable rsyslog service manually to see logs in /var/log/syslog
//sudo service rsyslog start

namespace settings {
    bool debug = true; //if true - print debug messages and don't transform into daemon
    int sleep_time = 0; //in seconds, if 0 (additional arg not supplied) then sleep is set to DEFAULT_SLEEP_TIME
    bool recursive = false; //store status of recursive mode (if true then daemon will copy all files in subdirectories)
    int big_file_mb = 5; //store size of big file in MB (when file is bigger than this value, it will be copied using mmap)

    atomic<bool> received_signal(false); //used to store if signal was received, if true then daemon wake up and reset it to false
    atomic<bool> daemon_busy(false); //used to prevent double daemon wake up (by signal)
    atomic<bool> daemon_awaiting_termination(false);
}

namespace utils {

    void display_usage(const string &path) {
        string usage = "Usage: " + path +
                       " sourcePath destinationPath [-d|--debug] [-R|--recursive] [-s=<sleep_time>|--sleep_time=<sleep_time>]\n"
                       "\n"
                       "Description:\n"
                       "    FileSyncDaemon is a program that synchronizes files between two directories. It can be run as a daemon process to continuously monitor the directories and automatically synchronize any changes.\n"
                       "\n"
                       "Arguments:\n"
                       "    sourcePath        The path to the source directory.\n"
                       "    destinationPath   The path to the destination directory.\n"
                       "\n"
                       "Options:\n"
                       "    -d, --debug       Enable debug mode.\n"
                       "    -R, --recursive   Synchronize directories recursively.\n"
                       "    -s, --sleep_time  The time in seconds to sleep between iterations. Default value is 10.\n"
                       "\n"
                       "Example usage:\n"
                       "    " + path + " /home/user/source /mnt/backup -R -s=5";
        cout << usage << endl;
    }

    string get_operation_name(Operation operation) {
        switch (operation) {
            case DAEMON_SLEEP:
                return "DAEMON_SLEEP";
            case DAEMON_INIT:
                return "DAEMON_INIT";
            case DAEMON_WAKE_UP_BY_SIGNAL:
                return "DAEMON_WAKE_UP_BY_SIGNAL";
            case DAEMON_WAKE_UP_DEFAULT_TIMER:
                return "DAEMON_WAKE_UP_DEFAULT_TIMER";
            case DAEMON_WAKE_UP_CUSTOM_TIMER:
                return "DAEMON_WAKE_UP_CUSTOM_TIMER";
            case SIGNAL_received:
                return "SIGNAL_received";
            case DAEMON_INIT_ERROR:
                return "DAEMON_INIT_ERROR";
            case DAEMON_WORK_INFO:
                return "DAEMON_WORK_INFO";
            case FILE_OPERATION_INFO:
                return "FILE_OPERATION_INFO";
            case FILE_OPERATION_ERROR:
                return "FILE_OPERATION_ERROR";
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

        log(FILE_OPERATION_ERROR, "Can't change modification time for file: " + path + " due to error: " +
                                  strerror(errno));
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
            //TODO add to, when critical error from write will appear we should stop copying and return false
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
            log(FILE_OPERATION_INFO, "File " + path + " removed");
            return true;
        }

        log(FILE_OPERATION_ERROR, "File " + path + " remove failed due to " + strerror(errno));
        return false;
    }

    bool directory_delete(const string &path) {
        if (rmdir(path.c_str()) == 0) {
            log(FILE_OPERATION_ERROR, "Directory " + path + " removed");
            return true;
        }

        log(FILE_OPERATION_INFO, "Directory " + path + " remove failed due to " + strerror(errno));
        return false;
    }

    bool directory_create(const string &path) {
        if (mkdir(path.c_str(), 0777) == 0) {
            log(FILE_OPERATION_INFO, "Directory " + path + " created");
            return true;
        }

        log(FILE_OPERATION_ERROR, "Directory " + path + " creation failed due to " + strerror(errno));
        return false;
    }

    bool is_directory_empty(const string &path) {
        DIR *dir = opendir(path.c_str());
        if (dir == nullptr) {
            log(FILE_OPERATION_ERROR, "Can't open directory " + path + " due to error: " + strerror(errno));
            return false;
        }

        struct dirent *entry = readdir(dir);
        //loop over all files inside directory, if there is at least one file return false (not including . and ..)
        while (entry != nullptr) {
            if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
                closedir(dir);
                return false;
            }
            entry = readdir(dir);
        }
        closedir(dir);
        return true;
    }

    void remove_empty_directories(const string &destination) {
        //recursively remove empty directories inside destination directory
        //loop over all directories inside destination directory

        DIR *dir = opendir(destination.c_str());
        if (dir == nullptr) {
            log(FILE_OPERATION_ERROR, "Can't open directory " + destination + " due to error: " + strerror(errno));
            return;
        }

        struct dirent *entry = readdir(dir);
        //iterate over all files inside directory, if entry is directory call this function again (recursion)
        while (entry != nullptr) {
            if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
                string path = destination + "/" + entry->d_name;

                if (is_a_directory(path)) {
                    remove_empty_directories(path);

                    //if directory is empty, remove it
                    if (is_directory_empty(path)) {
                        directory_delete(path);
                    }
                }
            }
            entry = readdir(dir);
        }
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
            log(FILE_OPERATION_ERROR, "Can't get file size for " + path + " due to error: " + strerror(errno));
            return 0;
        }

        return (size_t) file_stat.st_size;
    }

    bool file_copy(const FileInfo &source, const string &destination) {
        //create subdirectories if needed
        if (!create_subdirectories(destination)) {
            log(Operation::FILE_OPERATION_ERROR, "Failed to create subdirectories for file " + source.path + " to " +
                                                 destination + " due to " + strerror(errno) + " (errno: " +
                                                 to_string(errno) +
                                                 ")");
            return false;
        }

        //check if size is bigger than 5MB
        //if yes, then use mmap
        //in other case use normal file copy
        bool result;
        if (source.size > settings::big_file_mb * 1024 * 1024) {
            result = mmap_file_copy(source, destination);
        } else {
            result = read_write_file_copy(source.path, destination);
        }

        //if copy was successful, then change modification time
        if (result) {
            change_file_modification_time(destination, source.lastModified);
            log(Operation::FILE_OPERATION_INFO, "Successfully copied file " + source.path + " to " + destination);
        } else {
            log(Operation::FILE_OPERATION_ERROR,
                "Failed to copy file " + source.path + " to " + destination + " due to " +
                strerror(errno) + " (errno: " + to_string(errno) + ")");
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
}

namespace actions {

    //block thread for specified time until signal is received or time is up
    void handle_daemon_counter() {
        int counter = 0;
        while (counter < settings::sleep_time) {
            if (settings::received_signal) {
                settings::received_signal = false;
                utils::log(Operation::DAEMON_WAKE_UP_BY_SIGNAL, "Daemon wake up by signal");
                return;
            }
            sleep(1);
            counter++;
        }

        //check if default time is used
        if (settings::sleep_time == DEFAULT_SLEEP_TIME) {
            utils::log(Operation::DAEMON_WAKE_UP_DEFAULT_TIMER,
                       "Daemon wake up by timer with default time: " + to_string(DEFAULT_SLEEP_TIME) +
                       " seconds");
        } else {
            utils::log(Operation::DAEMON_WAKE_UP_CUSTOM_TIMER,
                       "Daemon wake up by timer with custom time: " + to_string(settings::sleep_time) +
                       " seconds");
        }
    }

    //parse additional arguments
    //--sleep_time=10 or -s=10
    //-R or --recursive
    //-d or --debug
    //-B:5 or --big-file-size:5
    void handle_additional_args_parse(const string &arg) {
        if (utils::string_contain(arg, "--sleep-time") || utils::string_contain(arg, "-s")) {
            try {
                string sleep_time_str = arg.substr(arg.find('=') + 1);
                settings::sleep_time = stoi(sleep_time_str);

                utils::log(Operation::DAEMON_INIT,
                           "Custom sleep time: " + to_string(settings::sleep_time) +
                           " seconds");
            } catch (exception &e) {
                cerr << "Failed to parse sleep time parameter " << arg << " due to: " << e.what() << endl;
                utils::log(Operation::DAEMON_INIT_ERROR,
                           "Failed to parse sleep time parameter " + arg + " due to: " + e.what());
                exit(-1);
            }
        }

        if (arg == "-R" || arg == "--recursive") {
            settings::recursive = true;
            utils::log(Operation::DAEMON_INIT, "Recursive mode enabled");
        }

        if (arg == "--debug" || arg == "-d") {
            settings::debug = true;
            utils::log(Operation::DAEMON_INIT, "Debug mode enabled using arg flag");
        }

        if (utils::string_contain(arg, "--big-file-size") || utils::string_contain(arg, "-B")) {
            try {
                string sleep_time_str = arg.substr(arg.find('=') + 1);
                settings::big_file_mb = stoi(sleep_time_str);

                utils::log(Operation::DAEMON_INIT,
                           "Custom big file size: " + to_string(settings::big_file_mb) + " MB");
            } catch (exception &e) {
                cerr << "Failed to parse big file size parameter " << arg << " due to: " << e.what() << endl;
                utils::log(Operation::DAEMON_INIT_ERROR,
                           "Failed to parse big file size parameter " + arg + " due to: " + e.what());
                exit(-1);
            }
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
    //handle SIGUSR1 signal
    //allow daemon to skip countdown and wake up immediately
    void sigusr1_signal_handler(int signum) {
        if (signum != SIGUSR1) return;

        //check if daemon is busy, if so, ignore signal
        if (settings::daemon_busy) {
            utils::log(Operation::SIGNAL_received, "Signal USR1 received, but daemon is busy");
            return;
        }

        //set settings received signal to true, so daemon can wake up
        utils::log(Operation::SIGNAL_received, "Signal USR1 received");
        settings::received_signal = true;
    }

    //handle SIGTERM signal
    //allow daemon to finish current iteration and then terminate
    void sigterm_signal_handler(int signum) {
        if (signum != SIGTERM) return;

        //set settings received signal to true, so daemon can wake up
        utils::log(Operation::SIGNAL_received, "Signal TERM received");
        settings::daemon_awaiting_termination = true;
    }

    //A demon lurks within my code,
    //Its cursed power seems to corrode.
    //With daemon_handler it will explode,
    //But C++ expertise will ease the load.
    [[noreturn]] void daemon_handler(const string &sourcePath, const string &destinationPath) {
        while (true) {
            vector<FileInfo> sourceDirFiles = {};
            vector<FileInfo> destinationDirFiles = {};
            string recursivePathCollector; //used to collect path to file in recursive mode, only used as help variable

            //check if daemon is awaiting termination
            if (settings::daemon_awaiting_termination) {
                utils::log(Operation::DAEMON_WORK_INFO, "Daemon awaiting termination - exiting");
                exit(0);
            }

            //block current thread until signal is received or sleep time is up
            //daemon logging about wake up event is handled in handle_daemon_counter
            actions::handle_daemon_counter();
            settings::daemon_busy = true;

            utils::scan_files_in_directory(sourcePath, settings::recursive, sourceDirFiles, destinationPath,
                                           recursivePathCollector);

            //check if source directory is empty
            //if so, skip this iteration
            if (sourceDirFiles.empty()) {
                utils::log(Operation::DAEMON_SLEEP, "No files found in source directory");
                settings::daemon_busy = false;
                continue;
            }

            utils::scan_files_in_directory(destinationPath, settings::recursive, destinationDirFiles, sourcePath,
                                           recursivePathCollector);

            utils::log(Operation::DAEMON_WORK_INFO, "Scanning directories finished, found " +
                                                    to_string(sourceDirFiles.size()) +
                                                    " files in source directory and " +
                                                    to_string(destinationDirFiles.size()) +
                                                    " files in destination directory");
            if (settings::debug) {
                cout << "Source directory files: \n";
                for (const auto &item: sourceDirFiles) {
                    cout << "Full path: " << item.path << "\nMirrored path: " << item.mirrorPath << "\nsize: "
                         << item.size
                         << "\nlast modified: " << item.lastModified << endl << endl;
                }

                cout << "Destination directory files: \n";
                for (const auto &item: destinationDirFiles) {
                    cout << "Full path: " << item.path << "\nMirrored path: " << item.mirrorPath << "\nsize: "
                         << item.size
                         << "\nlast modified: " << item.lastModified << endl << endl;
                }
            }

            //check if files in destination directory are not in source directory
            //if so, delete them
            for (const auto &file: destinationDirFiles) {
                //mirror path corresponds to source directory file
                //if file in destination directory is not in source directory, delete it
                if (utils::is_file_or_directory_exists(file.mirrorPath)) continue;

                //file not found in source directory, delete it
                utils::log(Operation::DAEMON_WORK_INFO,
                           "File " + file.path + " not found in source directory, deleting");
                utils::file_delete(file.path);
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
                //check if file is in destination directory, if not, copy it
                if (!utils::is_file_or_directory_exists(file.mirrorPath)) {
                    utils::log(Operation::DAEMON_WORK_INFO,
                               "File " + file.path + " not found in destination directory, copying");
                    utils::file_copy(file, file.mirrorPath);
                    continue;
                }

                //search for file struct in destination directory, which corresponds to current file path
                for (const auto &destinationFile: destinationDirFiles) {
                    if (file.mirrorPath == destinationFile.path) {

                        //check if files (source and destination) are the same
                        if (file.size != destinationFile.size || file.lastModified != destinationFile.lastModified) {
                            utils::log(Operation::DAEMON_WORK_INFO, "File " + file.path +
                                                                    " is different in source and destination directory, replacing");
                            utils::file_copy(file, file.mirrorPath);
                        }
                        break;
                    }
                }
            }

            //check if after removing files from destination directory, there are no empty directories left
            //if so, delete them
            utils::remove_empty_directories(destinationPath);

            //reset daemon busy flag
            settings::daemon_busy = false;
            utils::log(Operation::DAEMON_SLEEP, "Daemon finished file synchronization");
            exit(0);
        }
    }
}

bool transform_to_daemon() {
    pid_t pid = fork();

    //failed to fork so daemon can't be created
    if (pid < 0) {
        utils::log(Operation::DAEMON_INIT, "Failed to fork");
        return false;
    }

    //kill parent process
    if (pid > 0) {
        utils::log(Operation::DAEMON_INIT, "Daemon created");
        exit(EXIT_SUCCESS);
    }

    //create new session and process group
    if (setsid() < 0) {
        utils::log(Operation::DAEMON_INIT, "Failed to create new session");
        return false;
    }

    //ignore signals from terminal, we don't need them
    signal(SIGCHLD, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    //set signal handlers to our own functions
    signal(SIGUSR1, handlers::sigusr1_signal_handler);
    signal(SIGTERM, handlers::sigterm_signal_handler);

    pid = fork();
    //fork again, so parent process can exit
    if (pid < 0) {
        utils::log(Operation::DAEMON_INIT, "Failed to fork");
        return false;
    }

    //kill parent process
    if (pid > 0) {
        utils::log(Operation::DAEMON_INIT, "Daemon created");
        exit(EXIT_SUCCESS);
    }

    //set new file permissions
    if (umask(0) == -1) {
        utils::log(Operation::DAEMON_INIT, "Failed to set file permissions");
        return false;
    }

    //set working directory to root
    if (chdir("/") == -1) {
        utils::log(Operation::DAEMON_INIT, "Failed to change working directory to root");
        return false;
    }

    //close stdin, stdout and stderr
    if (close(STDIN_FILENO) == -1) {
        utils::log(Operation::DAEMON_INIT, "Failed to close stdin");
        return false;
    }
    if (close(STDOUT_FILENO) == -1) {
        utils::log(Operation::DAEMON_INIT, "Failed to close stdout");
        return false;
    }
    if (close(STDERR_FILENO) == -1) {
        utils::log(Operation::DAEMON_INIT, "Failed to close stderr");
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
        utils::log(Operation::DAEMON_INIT, "Failed to open /dev/null");
        return false;
    }

    if (dup2(fd, STDIN_FILENO) == -1) {
        utils::log(Operation::DAEMON_INIT, "Failed to redirect stdin to /dev/null");
        return false;
    }
    if (dup2(fd, STDOUT_FILENO) == -1) {
        utils::log(Operation::DAEMON_INIT, "Failed to redirect stdout to /dev/null");
        return false;
    }
    if (dup2(fd, STDERR_FILENO) == -1) {
        utils::log(Operation::DAEMON_INIT, "Failed to redirect stderr to /dev/null");
        return false;
    }

    //TODO ask if daemon() function can be used instead of this function
    return true;
}

int main(int argc, char *argv[]) {
    utils::log(Operation::DAEMON_INIT, "[*] File synchronization daemon started");

    if (argc < 3) {
        utils::display_usage(argv[0]);
        utils::log(Operation::DAEMON_INIT_ERROR, "Not enough arguments supplied, expected 3, got " + to_string(argc));
        return -1;
    }

    string sourcePath = argv[1];
    string destinationPath = argv[2];

    //verify input directories
    if (!actions::validate_input_dirs(sourcePath, destinationPath)) {
        return -1;
    }

    //<editor-fold desc="additional args parse">
    vector<string> additionalArgs;
    for (int i = 3; i < argc; i++) {
        additionalArgs.emplace_back(argv[i]);
    }

    for (const auto &item: additionalArgs) {
        actions::handle_additional_args_parse(item);
    }

    //fixup sleep time if additional arg was not supplied
    if (settings::sleep_time == 0) {
        settings::sleep_time = DEFAULT_SLEEP_TIME;
    }
    //</editor-fold>

    utils::log(Operation::DAEMON_INIT,
               "Daemon initialized with source path: " + sourcePath + " and destination path: " +
               destinationPath + " and sleep time: " + to_string(settings::sleep_time) + " seconds");

    //if debug mode is enabled, don't transform to daemon
    //and handle signals manually
    if (settings::debug) {
        signal(SIGUSR1, handlers::sigusr1_signal_handler);
        signal(SIGTERM, handlers::sigterm_signal_handler);
    } else {
        if (!transform_to_daemon()) {
            utils::log(Operation::DAEMON_INIT, "Failed to transform to daemon");
            return -1;
        }
    }

    handlers::daemon_handler(sourcePath, destinationPath);
}