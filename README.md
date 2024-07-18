# Author Information 

Name: Patrick Tuohy & Varchasvi Vaishnav Rampure

Email: tuohy2@wisc.edu

# Description 

In this project we implemented a file system utilizing FUSE (Filesystem in Userspace).  FUSE lets regular users build their own file systems without needing special permissions, opening up new possibilities for designing and using file systems. This filesystem handles basic tasks like reading, writing, making directories, deleting files, and more.

# Parts of FUSE implemented:
Create empty files and directories
Read and write to files, up to the maximum size supported by the indirect data block.
Read a directory (e.g. ls should work)
Remove an existing file or directory (presume directories are empty)
Get attributes of an existing file/directory
Fill the following fields of struct stat

st_uid
st_gid
st_atime
st_mtime
st_mode
st_size

