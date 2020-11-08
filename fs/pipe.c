/*
 *  linux/fs/pipe.c
 *
 *  (C) 1991  Linus Torvalds
 */
/*
 * 管道的本质是“一页内存”。
 * 操作系统在内存中为每个管道开辟一页内存，给这一页内存
 * 赋予了文件的属性。这一页内存由两个进程共享，但不会分配
 * 给任何进程，只有内核掌控。
 */

#include <signal.h>

#include <linux/sched.h>
#include <linux/mm.h>	/* for get_free_page */
#include <asm/segment.h>

int read_pipe(struct m_inode * inode, char * buf, int count)
{
	int chars, size, read = 0;

	while (count>0) {
		// 读写指针重合时，就视为把管道中数据都读完了
		while (!(size=PIPE_SIZE(*inode))) {
			// 管道数据都读完了，唤醒写管道进程
			wake_up(&inode->i_wait);

			if (inode->i_count != 2) /* are there any writers? */
				return read;

			// 没数据读了，将读管道进程挂起
			sleep_on(&inode->i_wait);
		}

		chars = PAGE_SIZE-PIPE_TAIL(*inode);
		if (chars > count)
			chars = count;
		if (chars > size)
			chars = size;

		count -= chars;
		read += chars;
		size = PIPE_TAIL(*inode);

		// 读多少数据，读指针就偏移多少
		PIPE_TAIL(*inode) += chars;

		// 指针超过一个页面，&=操作可以实现自动回滚
		PIPE_TAIL(*inode) &= (PAGE_SIZE-1);

		while (chars-->0)
			put_fs_byte(((char *)inode->i_size)[size++],buf++);
	}

	// 读取了数据，意味着管道有剩余空间了，唤醒管道进程
	wake_up(&inode->i_wait);

	return read;
}
	
int write_pipe(struct m_inode * inode, char * buf, int count)
{
	int chars, size, written = 0;

	while (count>0) {
		// 写指针最多能写入4095字节的数据，就视为把管道写满了
		while (!(size=(PAGE_SIZE-1)-PIPE_SIZE(*inode))) {

			// 管道写满了，有数据了，唤醒读管道进程
			wake_up(&inode->i_wait);

			if (inode->i_count != 2) { /* no readers */
				current->signal |= (1<<(SIGPIPE-1));
				return written?written:-1;
			}

			// 没剩余空间了，将写管道进程挂起
			sleep_on(&inode->i_wait);
		}

		chars = PAGE_SIZE-PIPE_HEAD(*inode);
		if (chars > count)
			chars = count;
		if (chars > size)
			chars = size;

		count -= chars;
		written += chars;
		size = PIPE_HEAD(*inode);

		// 写多少数据，读指针就偏移多少
		PIPE_HEAD(*inode) += chars;

		// 指针超过一个页面，&=操作可以实现自动回滚
		PIPE_HEAD(*inode) &= (PAGE_SIZE-1);

		while (chars-->0)
			((char *)inode->i_size)[size++]=get_fs_byte(buf++);
	}

	// 写入了数据，意味着管道有数据了，唤醒读管道进程
	wake_up(&inode->i_wait);

	return written;
}

int sys_pipe(unsigned long * fildes)
{
	struct m_inode * inode;
	struct file * f[2];
	int fd[2];
	int i,j;

	// 准备在file_table[64]中申请两个空闲项
	j=0;
	for(i=0;j<2 && i<NR_FILE;i++)
		if (!file_table[i].f_count) // 找到空闲项
			(f[j++]=i+file_table)->f_count++; // 为每项引用计数设置为1

	if (j==1)
		f[0]->f_count=0;
		
	if (j<2)
		return -1;

	// 准备在*filep[20]中申请两个空闲项
	j=0;
	for(i=0;j<2 && i<NR_OPEN;i++) 
		if (!current->filp[i]) { // 找到空闲项
			// 分别与file_table[64]中申请的两个空闲项建立关联
			current->filp[ fd[j]=i ] = f[j];
			j++;
		}

	if (j==1)
		current->filp[fd[0]]=NULL;

	if (j<2) {
		f[0]->f_count=f[1]->f_count=0;
		return -1;
	}

	// 创建管道文件inode
	if (!(inode=get_pipe_inode())) {
		current->filp[fd[0]] =
			current->filp[fd[1]] = NULL;

		f[0]->f_count = f[1]->f_count = 0;
		return -1;
	}

	f[0]->f_inode = f[1]->f_inode = inode;
	f[0]->f_pos = f[1]->f_pos = 0;
	f[0]->f_mode = 1;		/* read */
	f[1]->f_mode = 2;		/* write */

	put_fs_long(fd[0],0+fildes);
	put_fs_long(fd[1],1+fildes);

	return 0;
}
