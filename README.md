#PLP to C compiler
##Dependencies

Building PLPC requires GNU flex, GNU bison, gcc, and libarchive (git has been included because it is requried to clone this repository). The following has been tested on Ubuntu 14.04 LTS (64-bit), but should work on any Debian Linux distribution:

```bash
sudo apt-get install git flex bison gcc libarchive-dev
```


##Building

With that done, just call make from the top of the plpc tree, which should be the same place as this file.

If successful, you should have the entire toolchain in ${PLPC}/bin.

You'll need to add the plpc binary directory to your path.

##Testing

The makefile tries to run tests on a successful build, which requires that you have two environment variables set.
You need to have ${PLPC}/bin in your PATH variable, and you need to have $plptool set to "java -jar $PLPPATH/PLPTool.jar"
