#C to PLP Compiler
##Installation Guide

###1. Installing Dependancies
Building PLPC requires GNU flex, GNU bison, gcc, and libarchive (git has been included because it is requried to clone this repository). The following has been tested on Ubuntu 14.04 LTS (64-bit) using PLPTool 5.2 (link included in instructions), but should work on any Debian Linux distribution:

```bash
sudo apt-get install git flex bison gcc libarchive-dev
```


###2. Cloning C to PLP Compiler Repository
The following commands will clone a copy of this repository into your home directory. The remainder of these instructions assume it is cloned to your home directory.

```bash
cd ~
git clone https://github.com/Progressive-Learning-Platform/CLP.git
```


###3. Installing PLPTool
You will also need to have a copy of the PLPTool JAR file. The simplest way to get this on a Debian machine is to install the PLPTool Debian package (.deb), which can be found [here](https://github.com/Progressive-Learning-Platform/progressive-learning-platform/releases). For the remainder of this guide it will be assumed that you installed PLPTool using this method.


###4. Setting Environment Variables
The top level Makefile, located in the CLP directory, builds and tests the compiler. In order for this process to complete successfully two environment variables need to be set: the *bin* directory needs to be added to `$PATH` and `$plptool` needs to be set to the command used to run PLPTool. The simplest perminent solution to modify *.profile*, which can be done using the following command (this uses nano, but use whatever text editor you are most comfortable with):

```bash
nano ~/.profile
```

Add the following to the end of the file:

```bash
# add plptool environment variable
plptool="java -jar /usr/lib/plptool5/PLPToolStatic.jar"
export plptool

# add PLP compiler bin to PATH
export PATH=$PATH:$HOME/CLP/bin
```

Use `Ctrl+x` to exit nano, type `y` to modify the file, and hit `enter` to save with the same file name. You must log out (or restart) in order for these changes to take effect.



###5. Building and Testing Compiler
Now your system should be ready to build and test the compiler. After logging back, in navigate to the CLP directory:

```bash
cd ~/CLP
```

The compiler will be built and the automated test will be run using the following command:

```bash
make
```
