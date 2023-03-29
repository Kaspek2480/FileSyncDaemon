#include <iostream>

using namespace std;

int main(int argc, char *argv[]) {

    if (argc != 3) {
        cout << "Blad liczby arg" << endl;
        return -1;
    }

    cout << argv[2] << endl;

    cout << "Arg count: " << argc << endl;

    for (int i = 0; i < argc; ++i) {
        cout << "Arg " << i << ": " << argv[i] << endl;
    }

    return 0;
}