#ifndef CHATRA_READLINE_H
#define CHATRA_READLINE_H

#include <cstdio>
#include <vector>

static inline int rl_bind_key(int, int (*)()) {
	return 0;
}

static inline int rl_insert() {
	return 0;
}

static inline void stifle_history(int) {
}

static char* readline(const char* prompt) {
	static std::vector<char> buffer(1024);
	std::printf("%s", prompt);
	std::fflush(stdout);
	return std::fgets(buffer.data(), buffer.size(), stdin);
}

static char* add_history(char *) {
	return nullptr;
}


#endif //CHATRA_READLINE_H
