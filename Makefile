# Bot mẫu HEXUDON — C++17 (transport sandbox: stdin/stdout)
all:
	g++ -std=c++17 -O2 -Wall -o bot main.cpp

clean:
	rm -f bot bot.exe
