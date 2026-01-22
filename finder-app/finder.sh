#!/bin/sh

# Check for correct number of arguments
if [ $# -ne 2 ]
then
    echo Must pass in two arguments: a path to a directory and a search string
    exit 1
fi

filesdir=$1
searchstr=$2

# Check that the directory is on the file system
if [ ! -d $filesdir ]
then
    echo $filesdir does not represent a directory on the file system
    exit 1
fi

numfiles=$( grep -r -l $2 $1/* | wc -l)
numlines=$( grep -r $2 $1/* | wc -l)

echo The number of files are $numfiles and the number of matching lines are $numlines
