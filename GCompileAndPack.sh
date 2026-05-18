#!/bin/sh

# https://github.com/marcin-filipiak/bash_GCompileAndPack

params="`pkg-config --cflags --libs gtk+-3.0 webkit2gtk-4.0`"

# package name from control file
package_name=$(grep 'Package:' control | cut -d' ' -f2)

rm bin/$package_name
clear
echo "--------------GCompileAndPack--------------"
echo "-------------------------------------------"
echo "                COMPILATION"
echo "-------------------------------------------"
g++ -std=c++17 -Wall -O2 -o bin/$package_name src/$package_name.cpp $params  2> error_log.txt
# Sprawdzanie, czy plik error_log.txt jest pusty
if [ -s error_log.txt ]; then
  echo "---Errors during compilation---"
  # Wyświetlanie błędów na ekranie z paginacją
  more error_log.txt
  rm error_log.txt
fi  
echo "-------------------------------------------"
echo "             END OF COMPILATION"
echo "-------------------------------------------"

# Check if compiled file exists before run and asking to create a .deb package
if [ -f "bin/$package_name" ]; then
	echo "-------------------------------------------"
	echo "                RUN"
	echo "-------------------------------------------"
	chmod +x bin/$package_name
	./bin/$package_name
	echo "-------------------------------------------"
	echo "             END OF RUN"
	echo "-------------------------------------------"


	# deb makind, you need dpkg-deb
	read -p "Would you like to make a .deb? (y/n): " build_deb
		if [ "$build_deb" = "y" ]; then
		cd bin
		rm -rf todeb
		mkdir todeb
		cd todeb
		mkdir -p DEBIAN
		mkdir -p usr/bin
		mkdir -p etc
		cd ..
		cp $package_name todeb/usr/bin
		cp ../control todeb/DEBIAN
		dpkg-deb --build todeb
		rm -rf todeb
		mv todeb.deb ../$package_name.deb
	fi
fi

