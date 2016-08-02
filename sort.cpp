#include <iostream>
#include <vector>
#include <string>
//
int my_sort(void);
int get_str(void);
//
int main(void) {
	// Переключаемся на русский язык.
	setlocale(LC_ALL, "RUS");
	// Сортирока.
	//my_sort();
	// Работа со строками.
	get_str();
	int e; std::cin >> e;
	return 0;
}
int get_str(void) {
// Получить строку.
	std::string t;
	int k;
	std::cout << "Введите число k: ";
	// Так как мы указали число и нажали на Enter, то в стандартный ввод попал символ перевода строки '\n'.
	std::cin >> k;
	// Первый вызов функции std::getline() получит пустую строку до первого символа '\n', который пришол после ввода числа.
	std::getline(std::cin, t);
	// Второй вызов будет получать корректную строку (символ перевода строки в t не войдёт).
	std::getline(std::cin, t);
	std::cout << "Вы ввели строку: '" << t << "'" << std::endl;
	// Выводим строку по символьно.
	for (size_t i = 0; i < t.size(); i++)	{
		std::cout << "\"" << t[i] << "\" ";
	}
	std::cout << "\n";
	return 0;
}
int my_sort(void){
// Сортировка "сортировка выбором минимума".
	/*
	// Статический массив.
	std::vector <int> v = {1, 8, 3, 2, 1, 2, 1, 4};
	int f = 0;
	for (auto k : v) {
		std::cout << "v(" << f << ") =" << k << "\n";
		f++;
	}
	*/
	// Получаем массив из STDIN.
	int k, f;
	std::cout << "Укажите число элементов в массиве, k: ";
	std::cin >> k;
	std::vector <int> v;
	for (int i = 0; i < k; i++) {
		std::cin >> f;
		v.push_back(f);
	}
	// Сортировка.
	int min, min_index, tmp, tmp_index;
	for (int j = 0; j < v.size(); j++) {
		min_index = j;
		tmp_index = j;
		// Находим индекс минимального элемента.
		for (int i = j + 1; i < v.size(); i++) {
			if (v[i] < v[min_index]) {
				min_index = i;
			}
		}
		// Если необходимо, то выполняем перестановку текущего элемента с миниальным.
		if (tmp_index != min_index) {
			tmp = v[min_index];
			v[min_index] = v[j];
			v[j] = tmp;
		}
	}
	// Вывод.
	std::cout << "sort\n";
	f = 0;
	for (auto k : v) {
		std::cout << "v(" << f << ") =" << k << "\n";
		f++;
	}
	return 0;
}
