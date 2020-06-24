#!/bin/bash
#
# This script sets up a development environment on Ubuntu 18.04 and is adapted
# from the CI scripts under .github/workflows. Feel free to adjust these
# instructions for your distribution of choice.

set -eu

EXPECTED_RELEASE="bionic"
RELEASE=$(lsb_release --codename | awk '{print $2}')

if [[ "${RELEASE}" != "${EXPECTED_RELEASE}" ]]
then
  echo "This script was intended for ${EXPECTED_RELEASE} (you have ${RELEASE}) exiting!"
  exit 1
fi

# installing g{cc,++}-9
sudo add-apt-repository ppa:ubuntu-toolchain-r/test
sudo apt update
sudo apt install gcc-9 g++-9

# installing up-to-date cmake https://apt.kitware.com/
wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null \
  | gpg --dearmor - \
  | sudo tee /etc/apt/trusted.gpg.d/kitware.gpg >/dev/null
sudo apt-add-repository 'deb https://apt.kitware.com/ubuntu/ bionic main'
sudo apt-get update
sudo apt upgrade cmake
# alternatively:
#   pip install cmake

# installing arrow and parquet keyring
curl -fL --output /tmp/arrow-keyring.deb \
  https://apache.bintray.com/arrow/ubuntu/apache-arrow-archive-keyring-latest-$(lsb_release --codename --short).deb \
  && sudo apt install -yq /tmp/arrow-keyring.deb \
  && rm /tmp/arrow-keyring.deb
# installing arrow and parquet
sudo apt update
sudo apt install -yq libarrow-dev libparquet-dev

# installing up-to-date llvm
sudo apt-add-repository 'deb http://apt.llvm.org/bionic/ llvm-toolchain-bionic-10 main'
curl -fL https://apt.llvm.org/llvm-snapshot.gpg.key | sudo apt-key add -
sudo apt update
sudo apt install -yq clang-10 clang++-10 clang-format-10 clang-tidy-10 llvm-10-dev

# make clang-{tidy,format}-10 the default
sudo update-alternatives --verbose --install /usr/bin/clang-tidy clang-tidy /usr/bin/clang-tidy-10 90
sudo update-alternatives --verbose --install /usr/bin/clang-format clang-format /usr/bin/clang-format-10 90

#       Setup conan assuming you want build as a subdirectory in repository root
# .github/workflows/setup_conan.sh
# mkdir build
# conan install -if ./build --build=missing config

#    Run cmake in build
# cd build
# cmake ../ -DCMAKE_TOOLCHAIN_FILE=./conan_paths.cmake -DGALOIS_ENABLE_DIST=on