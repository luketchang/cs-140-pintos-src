#include "filesys/path.h"
#include <debug.h>
#include <stdio.h>
#include "filesys/filesys.h"
#include "threads/malloc.h"

/* Primarily called to open last parent subdirectory in a full 
   path. Converts string PATH to an open inode, which the caller 
   is responsible for closing. */
struct inode * 
path_to_inode (const char *path)
{    
  /* Make copy of path. */
  char *path_copy = malloc (strlen (path) + 1);
  if (path_copy == NULL)
    PANIC ("path_to_inode: memory allocation failed for path_copy");
    
  strlcpy (path_copy, path, strlen (path) + 1);

  /* If entire path is "/", return root inode. */
  if (path_copy[0] == '/' && strlen (path_copy) == 1)
    return inode_open (ROOT_DIR_SECTOR);

  char *token, *ptr;
  struct dir *dir = get_start_dir (path_copy);

  /* Traverse path. */
  struct inode *inode = NULL;
  for (token = strtok_r (path_copy, "/", &ptr); token != NULL; 
       token = strtok_r (NULL, "/", &ptr))
    {
      const struct dir *const_dir = (const struct dir *) dir;
      const char *const_name = (const char *) token;
      
      /* Synchronization in inode_read_at makes dir_lookup safe in
         context of potentially searching for dir_entry being removed. */
      if (dir_lookup (const_dir, const_name, &inode))
        {
          if (dir->inode->sector != ROOT_DIR_SECTOR)
            inode_close (dir->inode);
          dir->inode = inode;
          dir->pos = 0;
        }
      else
        return NULL;
    }
  
  return inode;
}

/* Gets the starting directory for a path to inode
   conversion. If first char isn't a slash, we use our
   cwd as starting point for conversion. */
struct dir *
get_start_dir (const char *path)
{
  struct dir *dir;
  if (path[0] == '/')
    dir = dir_open_root ();
  else
    dir = dir_open_cwd ();

  return dir;
}

/* Extract base (path minus final token). 
   Called in filesys_open() and filesys_remove(). */
char *
extract_base (const char *path)
{
  char *last_slash = strrchr (path, '/');
  if (last_slash == NULL)
    return NULL;
  
  intptr_t base_len = (intptr_t) last_slash - (intptr_t) path;
  char *base = NULL;

  /* If last slash isn't first slash, return base string. 
     Else, just return "/" as base. */
  if (base_len != 0)
    {
      base = malloc (sizeof (last_slash - path + 1));
      if (base == NULL)
        PANIC ("extract_base: malloc failed for base.");
      strlcpy (base, path, base_len + 1);
      base[base_len + 1] = '\0';
    }
  else
    base = "/";
  
  return base;
}

/* Extract file/directory name (final token).
   Called in filesys_open() and filesys_remove(). */
char *
extract_name (const char *path)
{
  char *last_slash = strrchr (path, '/');
  if (last_slash == NULL)
    return (char *) path;

  return last_slash + 1;
}

/* Remove leading slashes in path. */
char *
remove_leading_slashes (const char *path)
{
  char *path_root = (char *) path;
  if (path_root[0] == '/' && path_root[1] == '/')
    path_root++;
  
  return path_root;
}
