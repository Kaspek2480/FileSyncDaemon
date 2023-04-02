#include <iostream>
#include <filesystem>

using namespace std;

void changeSleepTime (string &sleep_time, string new_time) {

    sleep_time = new_time;
}

void sleepParCheck (string sleep_time) {

    if (sleep_time.empty()) {

        cout << "Nie podano parametru --sleep_time" << endl;
    } // Parametr --sleep_time nie został podany

    else {
        cout << "Parametr --sleep_time istnieje i wynosi: " << sleep_time << endl;
    }
}

int main(int argc, char *argv[]) {

    string sleepTime = "";

    if (argc != 3) {
        cerr << "Blad liczby argumentow" << endl;
        return -1;
    }

    string sourcePath = argv[1];

    if (filesystem::exists(sourcePath) == 0) {
        cout << "Katalog " << sourcePath << " nie istnieje" << endl;
        return -1;
    } //sprawdzanie czy katalogi istnieja

    string destinationPath = argv[2];

    if (filesystem::exists(destinationPath) == 0) {
        cout << "Katalog nie istnieje" << endl;
        return -1;
    } //sprawdzanie czy katalogi istnieja

    for (int i = 0; i < argc; i++) {
        if (fopen(argv[i], "r") == NULL) {
            cout << "Katalog nie istnieje" << endl;
            return -1;
        }
    } //sprawdzanie czy katalogi istnieja


    for (int i = 0; i < argc; i++) {
        if (filesystem::is_directory(argv[i]) == 0) {
            cout << "Podana sciezka nie jest katalogiem" << endl;
        } else {
            cout << "Podana sciezka jest katalogiem" << endl;
        }
    } //sprawdzenie czy podane sciezki sa katalogiem




    return 0;
}