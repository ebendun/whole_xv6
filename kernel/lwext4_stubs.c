#include "types.h"

#define CONFIG_USE_DEFAULT_CFG 1
#define CONFIG_USE_USER_MALLOC 1
#define CONFIG_HAVE_OWN_ERRNO 1
#define CONFIG_HAVE_OWN_OFLAGS 1
#define CONFIG_DEBUG_PRINTF 0
#define CONFIG_DEBUG_ASSERT 0
#define CONFIG_JOURNALING_ENABLE 0
#define CONFIG_XATTR_ENABLE 0

#include <stddef.h>
#include <string.h>
#include <ext4_errno.h>
#include <ext4_fs.h>
#include <ext4_xattr.h>

void
qsort(void *base, size_t nmemb, size_t size,
      int (*compar)(const void *, const void *))
{
  char *a = (char*)base;
  char tmp[64];

  if(size > sizeof(tmp))
    return;
  for(size_t i = 1; i < nmemb; i++){
    size_t j = i;
    while(j > 0 && compar(a + j * size, a + (j - 1) * size) < 0){
      memmove(tmp, a + j * size, size);
      memmove(a + j * size, a + (j - 1) * size, size);
      memmove(a + (j - 1) * size, tmp, size);
      j--;
    }
  }
}

const char *
ext4_extract_xattr_name(const char *full_name, size_t full_name_len,
                        uint8_t *name_index, size_t *name_len, bool *found)
{
  (void)full_name_len;
  if(name_index) *name_index = 0;
  if(name_len) *name_len = 0;
  if(found) *found = false;
  return full_name;
}

const char *
ext4_get_xattr_name_prefix(uint8_t name_index, size_t *ret_prefix_len)
{
  (void)name_index;
  if(ret_prefix_len) *ret_prefix_len = 0;
  return "";
}

int
ext4_xattr_list(struct ext4_inode_ref *inode_ref,
                struct ext4_xattr_list_entry *list, size_t *list_len)
{
  (void)inode_ref;
  (void)list;
  if(list_len) *list_len = 0;
  return EOK;
}

int
ext4_xattr_get(struct ext4_inode_ref *inode_ref, uint8_t name_index,
               const char *name, size_t name_len, void *buf, size_t buf_len,
               size_t *data_len)
{
  (void)inode_ref;
  (void)name_index;
  (void)name;
  (void)name_len;
  (void)buf;
  (void)buf_len;
  if(data_len) *data_len = 0;
  return ENOTSUP;
}

int
ext4_xattr_remove(struct ext4_inode_ref *inode_ref, uint8_t name_index,
                  const char *name, size_t name_len)
{
  (void)inode_ref;
  (void)name_index;
  (void)name;
  (void)name_len;
  return ENOTSUP;
}

int
ext4_xattr_set(struct ext4_inode_ref *inode_ref, uint8_t name_index,
               const char *name, size_t name_len, const void *value,
               size_t value_len)
{
  (void)inode_ref;
  (void)name_index;
  (void)name;
  (void)name_len;
  (void)value;
  (void)value_len;
  return ENOTSUP;
}
