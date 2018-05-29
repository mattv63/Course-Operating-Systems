/*
	FUSE: Filesystem in Userspace
	Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

	This program can be distributed under the terms of the GNU GPL.
	See the file COPYING.
*/

#define	FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>

//size of a disk block
#define	BLOCK_SIZE 512

//we'll use 8.3 filenames
#define	MAX_FILENAME 8
#define	MAX_EXTENSION 3

//How many files can there be in one directory?
#define MAX_FILES_IN_DIR (BLOCK_SIZE - sizeof(int)) / ((MAX_FILENAME + 1) + (MAX_EXTENSION + 1) + sizeof(size_t) + sizeof(long))

//The attribute packed means to not align these things
struct cs1550_directory_entry
{
	int nFiles;	//How many files are in this directory.
				//Needs to be less than MAX_FILES_IN_DIR

	struct cs1550_file_directory
	{
		char fname[MAX_FILENAME + 1];	//filename (plus space for nul)
		char fext[MAX_EXTENSION + 1];	//extension (plus space for nul)
		size_t fsize;					//file size
		long nStartBlock;				//where the first block is on disk
	} __attribute__((packed)) files[MAX_FILES_IN_DIR];	//There is an array of these

	//This is some space to get this to be exactly the size of the disk block.
	//Don't use it for anything.
	char padding[BLOCK_SIZE - MAX_FILES_IN_DIR * sizeof(struct cs1550_file_directory) - sizeof(int)];
} ;

typedef struct cs1550_root_directory cs1550_root_directory;

#define MAX_DIRS_IN_ROOT (BLOCK_SIZE - sizeof(int)) / ((MAX_FILENAME + 1) + sizeof(long))

struct cs1550_root_directory
{
	int nDirectories;	//How many subdirectories are in the root
						//Needs to be less than MAX_DIRS_IN_ROOT
	struct cs1550_directory
	{
		char dname[MAX_FILENAME + 1];	//directory name (plus space for nul)
		long nStartBlock;				//where the directory block is on disk
	} __attribute__((packed)) directories[MAX_DIRS_IN_ROOT];	//There is an array of these

	//This is some space to get this to be exactly the size of the disk block.
	//Don't use it for anything.
	char padding[BLOCK_SIZE - MAX_DIRS_IN_ROOT * sizeof(struct cs1550_directory) - sizeof(int)];
} ;


typedef struct cs1550_directory_entry cs1550_directory_entry;

//How much data can one block hold?
#define	MAX_DATA_IN_BLOCK (BLOCK_SIZE - sizeof(long))

struct cs1550_disk_block
{
	//The next disk block, if needed. This is the next pointer in the linked
	//allocation list
	long nNextBlock;

	//And all the rest of the space in the block can be used for actual data
	//storage.
	char data[MAX_DATA_IN_BLOCK];
};

#define MAX_FAT_ENTRIES (BLOCK_SIZE/sizeof(short))

struct cs1550_file_alloc_table_block {
	short table[MAX_FAT_ENTRIES];
};

typedef struct cs1550_disk_block cs1550_disk_block;

typedef struct cs1550_file_alloc_table_block cs1550_fat_block;

#define START_ALLOC_BLOCK 2

static cs1550_root_directory read_root(void);
static cs1550_fat_block read_fat(void);

static cs1550_root_directory read_root(){
	FILE* disk = fopen(".disk", "r+b");
	fseek(disk, 0, SEEK_SET);
	cs1550_root_directory root;
	fread(&root, BLOCK_SIZE, 1, disk);
	return root;
}

static cs1550_fat_block read_fat(){
	FILE* disk = fopen(".disk", "r+b");
	fseek(disk, BLOCK_SIZE, SEEK_SET);
	cs1550_fat_block fblock;
	fread(&fblock, BLOCK_SIZE, 1 , disk);
	return fblock;
}

static void write_root(cs1550_root_directory* root_on_disk){
	FILE* disk = fopen(".disk", "r+b");
	fwrite(root_on_disk, BLOCK_SIZE, 1, disk);
	fclose(disk);
}

static void write_fat(cs1550_fat_block* fat_on_disk){
	FILE* disk = fopen(".disk", "r+b");
	fseek(disk, BLOCK_SIZE, SEEK_SET);
	fwrite(fat_on_disk, BLOCK_SIZE, 1, disk);
	fclose(disk);
}
/*
 * Called whenever the system wants to know the file attributes, including
 * simply whether the file exists or not.
 *
 * man -s 2 stat will show the fields of a stat structure
 */
static int cs1550_getattr(const char *path, struct stat *stbuf)
{
	int res = 0;

	char directory[MAX_FILENAME + 1];
	char filename[MAX_FILENAME + 1];
	char extension[MAX_EXTENSION + 1];
	strcpy(directory, "");
	strcpy(filename, "");
	strcpy(extension, "");

	if (strlen(path) != 1){
		sscanf(path, "/%[^/]/%[^.].%s", directory, filename, extension);
	}

	memset(stbuf, 0, sizeof(struct stat));

	//is path the root dir?
	if (strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
		res = 0;
		return res;
	} else {
		if (strcmp(directory, "") == 0){
			res = -ENOENT;
			return res;
		}
		else{
			int i = 0;
			int directory_found = 0;
			struct cs1550_directory dir;

			cs1550_root_directory root = read_root();

			// Check to see if the given directory is one in the root
			for(i=0; i<root.nDirectories; i++){
				struct cs1550_directory current = root.directories[i];
				if(strcmp(current.dname, directory) == 0){
					dir = current;
					directory_found = 1;
					break;
				}
			}

			// No directory found so return error
			if (directory_found == 0){
				res = -ENOENT;
				return res;
			}

			// The path ends here so return the permissions of the directory
			if (strcmp(filename, "") == 0){
				res = 0;
				stbuf->st_mode = S_IFDIR | 0755;
				stbuf->st_nlink = 2;
				return res;
			}

			FILE* disk = fopen(".disk", "r+b");
			int location = BLOCK_SIZE*dir.nStartBlock;	// find location of directory on disk
			cs1550_directory_entry entry;

			fseek(disk, location, SEEK_SET);	//navigate to directory location
			entry.nFiles = 0;
			memset(entry.files, 0, MAX_FILES_IN_DIR*sizeof(struct cs1550_file_directory));
			int num_blocks = fread(&entry, BLOCK_SIZE, 1, disk);

			fclose(disk);

			if (num_blocks == 1){
				struct cs1550_file_directory f;
				int found_file = 0;
				int i;

				for(i=0;i<MAX_FILES_IN_DIR;i++){
					struct cs1550_file_directory current = entry.files[i];
					if(strcmp(current.fname, filename) == 0 && strcmp(current.fext, extension) == 0){
						f = current;
						found_file = 1;
						break;
					}
				}
				if (found_file == 0){
					res = -ENOENT;
					return res;
				}
				else {
					res = 0;
					stbuf->st_mode = S_IFREG | 0666;
					stbuf->st_nlink = 1;
					stbuf->st_size = f.fsize;
					return res;
				}
			}
		}
	}
	return res;
}

/*
 * Called whenever the contents of a directory are desired. Could be from an 'ls'
 * or could even be when a user hits TAB to do autocompletion
 */
static int cs1550_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi)
{
	//Since we're building with -Wall (all warnings reported) we need
	//to "use" every parameter, so let's just cast them to void to
	//satisfy the compiler
	(void) offset;
	(void) fi;

	//This line assumes we have no subdirectories, need to change
	if (strcmp(path, "/") != 0)
	return -ENOENT;

	//the filler function allows us to add entries to the listing
	//read the fuse.h file for a description (in the ../include dir)
	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);

	int length = strlen(path);
	char copy[length];
	strcpy(copy, path);

	char* dest = strtok(copy, "/");
	char* filename = strtok(NULL, ".");
	char* exten = strtok(NULL, ".");

	if (strcmp(path, "/") == 0){
		int i = 0;
		cs1550_root_directory root = read_root();

		// all directories in the root
		for(i=0;i<MAX_DIRS_IN_ROOT;i++){
			char* dir_name = root.directories[i].dname;
			if(strcmp(dir_name, "") != 0){
				filler(buf, dir_name, NULL, 0);
			}
		}
		return 0;
	}
	else {
		int i = 0;
		struct cs1550_directory current_dir;
		strcpy(current_dir.dname, "");
		current_dir.nStartBlock = -1;

		cs1550_root_directory root = read_root();

		for(i=0;i<MAX_DIRS_IN_ROOT;i++){
			if(strcmp(dest, root.directories[i].dname) == 0){
				current_dir = root.directories[i];
				break;
			}
		}

		if(strcmp(current_dir.dname, "") == 0){
			return -ENOENT;  //nothing was found in the root
		}
		else {
			FILE* disk = fopen(".disk", "rb+");
			int location = current_dir.nStartBlock*BLOCK_SIZE;
			fseek(disk, location, SEEK_SET);

			cs1550_directory_entry dir;
			dir.nFiles = 0;
			memset(dir.files, 0, MAX_FILES_IN_DIR*sizeof(struct cs1550_file_directory));

			fread(&dir, BLOCK_SIZE, 1, disk);
			fclose(disk);

			int j;
			for(j=0;j<MAX_FILES_IN_DIR; j++){
				struct cs1550_file_directory file_dir = dir.files[j];
				char f_copy[MAX_FILENAME+1];
				strcpy(f_copy, file_dir.fname);
				if(strcmp(file_dir.fext, "") != 0){
					strcat(f_copy, ".");
				}
				strcat(f_copy, file_dir.fext);
				if(strcmp(file_dir.fname, "") != 0){
					filler(buf, f_copy, NULL, 0);
				}
			}
		}
	}

	return 0;
}

/*
 * Creates a directory. We can ignore mode since we're not dealing with
 * permissions, as long as getattr returns appropriate ones for us.
 */
static int cs1550_mkdir(const char *path, mode_t mode)
{
	(void) path;
	(void) mode;

	char* dir;
	char* sub_dir;

	int length = strlen(path);
	char copy[length];
	strcpy(copy, path);

	dir = strtok(copy, "/");
	sub_dir = strtok(NULL, "/");

	if(strlen(dir)>MAX_FILENAME){ // Name of the directory is too long
		return -ENAMETOOLONG;
	}
	else if(sub_dir && sub_dir[0]){ // The user passsed in a sub directory when it should have been a file
		return -EPERM;
	}

	cs1550_root_directory root = read_root();
	cs1550_fat_block fblock = read_fat();

	// directories list is full
	if(root.nDirectories >= MAX_DIRS_IN_ROOT){
		return -EPERM;
	}

	int i;
	for(i=0; i<MAX_DIRS_IN_ROOT; i++){
		if(strcmp(root.directories[i].dname, dir) == 0){ //the directory trying to be created already exists
			return -EEXIST;
		}
	}

	for(i=0;i<MAX_DIRS_IN_ROOT;i++){
		if(strcmp(root.directories[i].dname, "") == 0){
			struct cs1550_directory new_dir;
			strcpy(new_dir.dname, dir);

			int j = 0;
			for(j = START_ALLOC_BLOCK; j < MAX_FAT_ENTRIES; j++){ // find new block to store directory
				if(fblock.table[j] == 0){
					fblock.table[j] = EOF;
					new_dir.nStartBlock = j;
					break;
				}
			}

			FILE* disk = fopen(".disk", "r+b");
			int location = BLOCK_SIZE*new_dir.nStartBlock;
			fseek(disk, location, SEEK_SET);
			cs1550_directory_entry entry;
			entry.nFiles = 0;
			int total = fread(&entry, BLOCK_SIZE, 1, disk);

			if(total == 1){
				memset(&entry, 0, sizeof(struct cs1550_directory_entry));
				fclose(disk);

				root.nDirectories++;
				root.directories[i] = new_dir;

				write_root(&root);
				write_fat(&fblock);
			}
			else {
				fclose(disk);
			}
			return 0;
		}
	}

	return 0;
}

/*
 * Removes a directory.
 */
static int cs1550_rmdir(const char *path)
{
	(void) path;
    return 0;
}

/*
 * Does the actual creation of a file. Mode and dev can be ignored.
 *
 */
static int cs1550_mknod(const char *path, mode_t mode, dev_t dev)
{
	(void) mode;
	(void) dev;

	char* dir;
	char* file_name;
	char* file_ext;

	int length = strlen(path);
	char copy[length];
	strcpy(copy, path);

	dir = strtok(copy, "/");
	file_name = strtok(NULL, ".");
	file_ext = strtok(NULL, ".");

	if ((dir && dir[0]) && strcmp(dir, "") != 0){
		if(file_name && file_name[0]){
			if(strcmp(file_name, "") == 0){
				return -EPERM;
			}

			if(file_ext && file_ext[0]){
				if(strlen(file_name) > MAX_FILENAME || strlen(file_ext) > MAX_EXTENSION){
					return -ENAMETOOLONG;
				}
			}
				else{
					if(strlen(file_name) > MAX_FILENAME){
						return -ENAMETOOLONG;
					}
				}
			}
			else {
				return -EPERM;
			}

			cs1550_root_directory root = read_root();
			cs1550_fat_block fblock = read_fat();

			struct cs1550_directory here;

			int i;
			for(i=0;i<MAX_DIRS_IN_ROOT;i++){
				struct cs1550_directory current = root.directories[i];
				if(strcmp(dir, current.dname) == 0){
					here = current;
					break;
				}
			}

			if(strcmp(here.dname, "") != 0){
				long directory_location = BLOCK_SIZE*here.nStartBlock;
				FILE* disk = fopen(".disk", "r+b");
				fseek(disk, directory_location, SEEK_SET);

				cs1550_directory_entry entry;
				int available = fread(&entry, BLOCK_SIZE, 1, disk);

				if(entry.nFiles >= MAX_FILES_IN_DIR){
					return -EPERM;
				}

				if(available){
					int exists = 0;
					int index = -1;

					int j;
					for(j=0;j<MAX_FILES_IN_DIR;j++){
						struct cs1550_file_directory current = entry.files[j];
						if(strcmp(current.fname, "") == 0 && strcmp(current.fext, "") == 0 && index == -1){
							index = j;
						}

						if(strcmp(current.fname, file_name) == 0 && strcmp(current.fext, file_ext) == 0){
							exists = 1;
							break;
						}
					}

					if(!exists){
						short fat_index = -1;

						int m;
						for(m=2;m<MAX_FAT_ENTRIES;m++){
							if(fblock.table[m] == 0){
								fat_index = m;
								fblock.table[m] = EOF;
								break;
							}
						}

						struct cs1550_file_directory new_dir;
						strcpy(new_dir.fname, file_name);
						if(file_ext && file_ext[0]){
							strcpy(new_dir.fext, file_ext);
						}
						else{
							strcpy(new_dir.fext, "");
						}
						new_dir.fsize = 0;
						new_dir.nStartBlock = fat_index;

						entry.files[index] = new_dir;
						entry.nFiles++;

						fseek(disk, directory_location, SEEK_SET);
						fwrite(&entry, BLOCK_SIZE, 1, disk);

						fclose(disk);

						write_root(&root);
						write_fat(&fblock);
					}
					else{
						fclose(disk);
						return -EEXIST;
					}
				}
				else{
					fclose(disk);
					return -EPERM;
				}
			}
			else{
				if(strcmp(dir, "") == 0){
					return 0;
				}
				else if(strcmp(file_name, "") == 0){
					return -EPERM;
				}
			}
	}
	return 0;
}

/*
 * Deletes a file
 */
static int cs1550_unlink(const char *path)
{
    (void) path;

    return 0;
}

/*
 * Read size bytes from file into buf starting from offset
 *
 */
static int cs1550_read(const char *path, char *buf, size_t size, off_t offset,
			  struct fuse_file_info *fi)
{
	(void) buf;
	(void) offset;
	(void) fi;
	(void) path;

	char* dir;
	char* file_name;
	char* file_ext;

	int length = strlen(path);
	char copy[length];
	strcpy(copy, path);

	dir = strtok(copy, "/");
	file_name = strtok(NULL, ".");
	file_ext = strtok(NULL, ".");


	if((dir && dir[0]) && strcmp(dir, "") != 0){ //there is a directory
		if(file_name && file_name[0]){
			if(strcmp(file_name, "") == 0){ //check for file
				return -EEXIST; //no file
			}

			if(file_ext && file_ext[0]){	// there is a file extension
				if(strlen(file_name) > MAX_FILENAME || strlen(file_ext) > MAX_EXTENSION){
					return -ENAMETOOLONG;
				}
			}
			else {
				if(strlen(file_name) > MAX_FILENAME){ //check if file name is over the limit
					return -ENAMETOOLONG;
				}
			}
		}
		else {
			return -EEXIST;
		}

		cs1550_root_directory root = read_root();
		cs1550_fat_block fblock = read_fat();

		struct cs1550_directory here;

		int i;
		for(i=0;i<MAX_DIRS_IN_ROOT;i++){ // find the right directory
			struct cs1550_directory current = root.directories[i];
			if(strcmp(dir, current.dname) == 0){
				here = current;
				break;
			}
		}

		if(strcmp(here.dname, "") != 0){ //the correct directory was found
			long location = BLOCK_SIZE*here.nStartBlock;
			FILE* disk = fopen(".disk", "r+b");
			fseek(disk, location, SEEK_SET);

			cs1550_directory_entry entry;
			int available = fread(&entry, BLOCK_SIZE, 1, disk);

			fclose(disk);

			if(available){ //we read in a directory entry
				struct cs1550_file_directory f;

				int j;
				for(j=0;j<MAX_FILES_IN_DIR;j++){ //search directory to find the file
					struct cs1550_file_directory cur_dir = entry.files[j];

					if(strcmp(cur_dir.fname, file_name) == 0){ //found the file
						if(file_ext && file_ext[0]){
							if(strcmp(cur_dir.fext, file_ext) == 0){ //extension match
								f = cur_dir;
								break;
							}
						}
						else {
							if(strcmp(cur_dir.fext, "") == 0){
								f = cur_dir;
								break;
							}
						}
					}
				}

				if(strcmp(f.fname, "") != 0){
					if(offset > f.fsize){
						return -EFBIG;
					}

					// find the correct block
					int block_num = 0;
					if(offset != 0){
						block_num = offset/BLOCK_SIZE;
					}

					int block_offset;
					if(offset!=0){
						block_offset = offset - block_num*BLOCK_SIZE;
					}

					int current_block = f.nStartBlock;
					if(block_num != 0){
						while (block_num > 0){
							current_block = fblock.table[current_block];
							block_num--;
						}
					}

					// open up the disk to read the data
					FILE* disk = fopen(".disk", "r+b");
					fseek(disk, BLOCK_SIZE*current_block+block_offset, SEEK_SET);
					cs1550_disk_block data;
					fread(&data.data, BLOCK_SIZE-block_offset, 1, disk);
					int current_size = 0;

					if(f.fsize >= BLOCK_SIZE){
						memcpy(buf, &data.data, BLOCK_SIZE-block_offset);
					}
					else {
						memcpy(buf, &data.data, f.fsize);
					}

					current_size = BLOCK_SIZE - block_offset;

					while(fblock.table[current_block] != EOF){
						current_block = fblock.table[current_block];
						cs1550_disk_block current_data;
						fseek(disk, BLOCK_SIZE*current_block, SEEK_SET);
						fread(&current_data.data, BLOCK_SIZE, 1, disk);
						memcpy(buf+current_size, &current_data, strlen(current_data.data));
						current_size += strlen(current_data.data);
					}

					fclose(disk);

					write_root(&root);
					write_fat(&fblock);

					size = current_size;
				}
				else{
					return -EISDIR;
				}
			}
			else {
				return -EPERM;
			}
		}
		else {
			if(strcmp(dir, "") == 0){
				return 0;
			}
			else if(strcmp(file_name, "") == 0){
				return -EPERM;
			}
		}
	}
	//check to make sure path exists
	//check that size is > 0
	//check that offset is <= to the file size
	//read in data
	//set size and return, or error

	return size;
}

/*
 * Write size bytes from buf into file starting from offset
 *
 */
static int cs1550_write(const char *path, const char *buf, size_t size,
			  off_t offset, struct fuse_file_info *fi)
{
	(void) buf;
	(void) offset;
	(void) fi;
	(void) path;

	char* dir;
	char* file_name;
	char* file_ext;

	int length = strlen(path);
	char copy[length];
	strcpy(copy, path);

	dir = strtok(copy, "/");
	file_name = strtok(NULL, ".");
	file_ext = strtok(NULL, ".");

	if((dir && dir[0]) && strcmp(dir, "") != 0){
		if(file_name && file_name[0]){
			if(strcmp(file_name, "") == 0){
				return -EEXIST;
			}

			if(file_ext && file_ext[0]){
				if(strlen(file_name) > MAX_FILENAME || strlen(file_ext) > MAX_EXTENSION){
					return -ENAMETOOLONG;
				}
			}
			else{
				if(strlen(file_name) > MAX_FILENAME){
					return -ENAMETOOLONG;
				}
			}
		}
		else {
			return -EEXIST;
		}

		cs1550_root_directory root = read_root();
		cs1550_fat_block fblock = read_fat();

		struct cs1550_directory here;

		int i;
		for(i=0; i<MAX_DIRS_IN_ROOT; i++){
			struct cs1550_directory current = root.directories[i];
			if(strcmp(dir, current.dname) == 0){
				here = current;
				break;
			}
		}

		if(strcmp(here.dname, "") != 0){
			long location = BLOCK_SIZE*here.nStartBlock;\

			FILE* disk = fopen(".disk", "r+b");
			fseek(disk, location, SEEK_SET);
			cs1550_directory_entry entry;
			int available = fread(&entry, BLOCK_SIZE, 1, disk);
			fclose(disk);

			if(available){
				struct cs1550_file_directory f;
				int index = -1;

				int j;
				for(j=0; j<MAX_FILES_IN_DIR;j++){
					struct cs1550_file_directory current_dir = entry.files[j];

					if(strcmp(current_dir.fname, file_name) == 0){
						if(file_ext && file_ext[0]){
							if(strcmp(current_dir.fext, file_ext) == 0){
								f = current_dir;
								index = j;
								break;
							}
						}
						else {
							if(strcmp(current_dir.fext, "") == 0){
								f = current_dir;
								index = j;
								break;
							}
						}
					}
				}

				if(strcmp(f.fname, "") != 0){
					if(offset>f.fsize){
						return -EFBIG;
					}
					int buffer_size = strlen(buf);
					int room = f.fsize - offset;

					int block_num = 0;
					if(offset != 0){
						block_num = offset/BLOCK_SIZE;
					}

					int block_offset = 0;
					if(offset != 0){
						block_offset = offset - block_num*BLOCK_SIZE;
					}

					int current_block = f.nStartBlock;
					if(block_num != 0){
						while(block_num > 0){
							current_block = fblock.table[current_block];
							block_num--;
						}
					}

					int bytes_left = buffer_size;

					FILE* disk = fopen(".disk", "r+b");
					fseek(disk, BLOCK_SIZE*current_block+block_offset, SEEK_SET);
					if(buffer_size >= BLOCK_SIZE){
						fwrite(buf, BLOCK_SIZE-block_offset, 1, disk);
						bytes_left -= (BLOCK_SIZE-block_offset);

						if(offset == size){
							f.fsize = offset=1;
						}
					}
					else {
						char x[BLOCK_SIZE-buffer_size];
						int m;
						for(m=0;m<BLOCK_SIZE-buffer_size; m++){
							x[m] = '\0';
						}
						fwrite(x, BLOCK_SIZE-buffer_size, 1, disk);
						bytes_left -= buffer_size;

						if(f.fsize > size){
							if(offset == size){
								f.fsize = offset+1;
							}
							else {
								f.fsize = size;
							}
						}
					}


					while(bytes_left > 0){
						if(fblock.table[current_block] == EOF){
							int free_block = 0;
							int k;
							for(k=2;k<MAX_FAT_ENTRIES;k++){
								if(fblock.table[k] == 0){
									fblock.table[current_block] = k;
									fblock.table[k] = EOF;
									current_block = k;
									free_block = 1;
									break;
								}
							}

							if(!free_block){
								return -EPERM;
							}
						}
						else {
							current_block = fblock.table[current_block];
						}

						fseek(disk, BLOCK_SIZE*current_block, SEEK_SET);
						if(bytes_left>=BLOCK_SIZE){
							char* new_address = buf = (buffer_size - bytes_left);
							fwrite(new_address, BLOCK_SIZE, 1, disk);
							bytes_left -= BLOCK_SIZE;
						}
						else {
							char* new_address = buf + (buffer_size - bytes_left);
							fwrite(new_address, bytes_left, 1, disk);
							bytes_left = 0;
						}
					}
					int write_bytes = buffer_size - bytes_left;
					int append_bytes = write_bytes - room;
					if(append_bytes > 0){
						f.fsize += append_bytes;
					}

					entry.files[index] = f;
					fseek(disk, here.nStartBlock*BLOCK_SIZE, SEEK_SET);
					fwrite(&entry, BLOCK_SIZE, 1, disk);

					fclose(disk);

					write_root(&root);
					write_fat(&fblock);

					size = buffer_size;
				}
				else {
					return -EISDIR;
				}
			}
			else {
				return -EPERM;
			}
		}
		else {
			if(strcmp(dir, "") == 0){
				return 0;
			}
			else if(strcmp(file_name, "") == 0){
				return -EPERM;
			}
		}
	}

	return size;
}

/******************************************************************************
 *
 *  DO NOT MODIFY ANYTHING BELOW THIS LINE
 *
 *****************************************************************************/

/*
 * truncate is called when a new file is created (with a 0 size) or when an
 * existing file is made shorter. We're not handling deleting files or
 * truncating existing ones, so all we need to do here is to initialize
 * the appropriate directory entry.
 *
 */
static int cs1550_truncate(const char *path, off_t size)
{
	(void) path;
	(void) size;

    return 0;
}


/*
 * Called when we open a file
 *
 */
static int cs1550_open(const char *path, struct fuse_file_info *fi)
{
	(void) path;
	(void) fi;
    /*
        //if we can't find the desired file, return an error
        return -ENOENT;
    */

    //It's not really necessary for this project to anything in open

    /* We're not going to worry about permissions for this project, but
	   if we were and we don't have them to the file we should return an error

        return -EACCES;
    */

    return 0; //success!
}

/*
 * Called when close is called on a file descriptor, but because it might
 * have been dup'ed, this isn't a guarantee we won't ever need the file
 * again. For us, return success simply to avoid the unimplemented error
 * in the debug log.
 */
static int cs1550_flush (const char *path , struct fuse_file_info *fi)
{
	(void) path;
	(void) fi;

	return 0; //success!
}


//register our new functions as the implementations of the syscalls
static struct fuse_operations hello_oper = {
    .getattr	= cs1550_getattr,
    .readdir	= cs1550_readdir,
    .mkdir	= cs1550_mkdir,
	.rmdir = cs1550_rmdir,
    .read	= cs1550_read,
    .write	= cs1550_write,
	.mknod	= cs1550_mknod,
	.unlink = cs1550_unlink,
	.truncate = cs1550_truncate,
	.flush = cs1550_flush,
	.open	= cs1550_open,
};

//Don't change this.
int main(int argc, char *argv[])
{
	return fuse_main(argc, argv, &hello_oper, NULL);
}
