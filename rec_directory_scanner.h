//
// Created by Kaspek on 03.04.2023.
//

#include <dirent.h>

void scan(string path) {
    //use C function to scan directory
    DIR *dir = opendir(path.c_str());
    if (dir == nullptr) {
        cout << "Error opening directory: " << path << endl;
        return;
    }

   /* struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
        //skip . and ..
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        //if directory
        if (ent->d_type == DT_DIR) {
            //scan directory
            scan(path + "/" + ent->d_name);
        } else {
            //if file
            //do something with file
            cout << path + "/" + ent->d_name << endl;
        }
    }*/
}
