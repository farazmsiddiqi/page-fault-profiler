#define LINUX

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/proc_fs.h>
#include <linux/list.h>
#include <linux/limits.h>
#include <uapi/linux/sched/types.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/cdev.h>
#include "mp3_given.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("farazms2");
MODULE_DESCRIPTION("CS-423 MP3");

#define DEBUG 1
#define TIMEOUT_MS 50
#define VBUFF_SIZE 128 * PAGE_SIZE
#define MAJOR_CDEV_NUM 423
#define MINOR_CDEV_NUM 0
#define STATUS "status"
#define PROC_NAME "mp3"

static const struct proc_ops proc_fops = {
   .proc_read = read_callback,
   .proc_write = write_callback,
};

static const struct file_operations cdev_fops = {
   .open = open_cdev_callback,
   .mmap = mmap_cdev_callback,
   .release = close_cdev_callback
};

static struct proc_dir_entry *proc_parent;
static struct proc_dir_entry *proc_status;

struct linked_list {
   struct list_head head;
   struct task_struct* linux_task;
   pid_t pid;
   unsigned long process_utilization;
   unsigned long major_fault_count;
   unsigned long minor_fault_count;
};

static bool wq_job_created = false;
static size_t ll_size = 0;

unsigned long* v_buff = NULL;
static unsigned int v_idx = 0;

struct cdev cdev;
dev_t dev = MKDEV(MAJOR_CDEV_NUM, 0);

struct linked_list ll_head;


// DEFINE_MUTEX(ll_lock);
DEFINE_SPINLOCK(ll_sp_lock);

static struct workqueue_struct *own_wq = NULL;
static DECLARE_DELAYED_WORK(work, workqueue_fn);

ssize_t read_callback(struct file * fp, char __user * usr_str, size_t n, loff_t * seek) {
   char *buffer = kzalloc(n, GFP_KERNEL);
   struct list_head *iter;
   int bytes_written = 0;
   size_t buf_len;
   size_t num_bytes_NOT_copied;

   spin_lock(&ll_sp_lock);
   list_for_each(iter, &ll_head.head) {
      pid_t pid = list_entry(iter, struct linked_list, head)->pid;
      unsigned long process_utilization = list_entry(iter, struct linked_list, head)->process_utilization;
      unsigned int major_fault_count = list_entry(iter, struct linked_list, head)->major_fault_count;
      unsigned int minor_fault_count = list_entry(iter, struct linked_list, head)->minor_fault_count;
      bytes_written += sprintf(buffer + bytes_written, "%d: %ld, %d, %d\n", pid, process_utilization, major_fault_count, minor_fault_count);
      printk(KERN_INFO "wrote %d: %ld, %d, %d to buffer\n", pid, process_utilization, major_fault_count, minor_fault_count);
   }
   spin_unlock(&ll_sp_lock);

   buf_len = strlen(buffer);

   if (*seek >= buf_len || *seek > 0) {
      return 0; // because you didnt copy anything in this case
   }

   // can replace with simple read
   num_bytes_NOT_copied = copy_to_user(usr_str, buffer, buf_len);
   if (num_bytes_NOT_copied < 0) {
      seek = seek + (n - num_bytes_NOT_copied);
      printk(KERN_ALERT "buffer not fully copied to usr_str on read_callback!");
      return -ENOMEM;
   }

   kfree(buffer);
   *seek += buf_len - num_bytes_NOT_copied;

   return buf_len - num_bytes_NOT_copied;
}

ssize_t write_callback(struct file * fp, const char __user * usr_str, size_t n, loff_t * seek) {
   char buffer[250] = {0};
   size_t offset;
   *seek = 0;

   offset = simple_write_to_buffer(buffer, 250, seek, usr_str, n);
   printk(KERN_INFO "Write callback triggered. Recieved input from user: %s", buffer);

   if (buffer[0] == 'R') {
      // on registration of process, add to linked list
      printk(KERN_INFO "recieved R (registration) in write_callback");
      register_node(fp, buffer, n, seek);
   } else if (buffer[0] == 'U') {
      // on deregistration, remove from linked list
      printk(KERN_INFO "recieved D (de-registration) in write_callback");
      deregister_node(fp, buffer, n, seek);
   }

   return offset;
}


static int open_cdev_callback(struct inode *inode, struct file *file) {
   return 0;
}


static int close_cdev_callback(struct inode *inode, struct file *file) {
   return 0;
}


static int mmap_cdev_callback(struct file *filp, struct vm_area_struct *vma) {
   int i;
   unsigned long pfn;

   if (vma->vm_end - vma->vm_start > VBUFF_SIZE) {
      printk(KERN_ALERT "failed to perform mmap: size exceeded");
      return -1;
   }

   for (i = 0; i < vma->vm_end - vma->vm_start; i += PAGE_SIZE) {
      pfn = vmalloc_to_pfn((void*)(((unsigned long)v_buff) + i));
      if(remap_pfn_range(vma, vma->vm_start+i, pfn, PAGE_SIZE, vma->vm_page_prot)){
			printk(KERN_ALERT "failed to perform mmap: remap");
			return -ENOMEM;
		}
   }
   return 0;
}


struct linked_list* find_node_in_ll(pid_t pid) {
   struct list_head *iter, *tmp;
   list_for_each_safe(iter, tmp, &ll_head.head) {
      struct linked_list* cur_node = list_entry(iter, struct linked_list, head);
      if (pid == cur_node->pid) {
         return cur_node;
      }
   }

   printk(KERN_INFO "Failed to find node with pid: %d in linked list.", pid);
   
   // NULL return is checked for later on.
   return NULL;
}


void deregister_node(struct file* fp, char* buffer, size_t n, loff_t* seek) {
   // get pid of process to be deregistered
   char c;
   pid_t pid;
   size_t vars_populated = sscanf(buffer, "%c %d", &c, &pid);
   struct list_head *iter, *tmp;

   if (vars_populated != 2) {
      printk(KERN_ALERT "DEREG SSCANF FAILURE: variables processed=%zu", vars_populated);
   }
   printk(KERN_INFO "variables processed. pid: %d", pid);

   // find node, delete it.
   spin_lock(&ll_sp_lock);
   list_for_each_safe(iter, tmp, &ll_head.head) {
      struct linked_list* cur_node = list_entry(iter, struct linked_list, head);
      if (pid == cur_node->pid) {
         del_node(iter);
         printk(KERN_INFO "Deregistered node with pid: %d", pid);
         spin_unlock(&ll_sp_lock);
         return;
      }
   }

   printk(KERN_INFO "node with pid: %d does not exist in linked list", pid);
   spin_unlock(&ll_sp_lock);

   // if, after node deletion, PCB linked list is empty, then delete work queue job as well.
}


void register_node(struct file * fp, char* buffer, size_t n, loff_t * seek) {
   // parse usr_str to get pid, process_utilization, process_time
   char c;
   pid_t pid;
   struct linked_list* ll_entry;
   size_t vars_populated = sscanf(buffer, "%c %d", &c, &pid);

   if (vars_populated != 2) {
      printk(KERN_ALERT "REG SSCANF FAILURE: variables processed=%zu", vars_populated);
   }
   printk(KERN_INFO "variables processed. pid: %d", pid);

   ll_entry = create_registration_node(pid);

   spin_lock(&ll_sp_lock);
   list_add_tail(&ll_entry->head, &ll_head.head);
   ll_size += 1;
   spin_unlock(&ll_sp_lock);

   // if node registration turns the PCB ll size from 0 to 1, then create work queue job.
   if (!wq_job_created) {
      printk(KERN_INFO "creating wq job");
      // sends interrupt after delay expires, and queues work, which will trigger workqueue_fn.
      queue_delayed_work(own_wq, &work, msecs_to_jiffies(TIMEOUT_MS));

      spin_lock(&ll_sp_lock);
      wq_job_created = true;
      spin_unlock(&ll_sp_lock);
   } 
}


static void workqueue_fn(struct work_struct *w) {
   // printk(KERN_INFO "triggered workqueue fn");
   spin_lock(&ll_sp_lock);
   unsigned long sum_mjr_flt, sum_min_flt, sum_proc_util;
   sum_mjr_flt = sum_min_flt = sum_proc_util = 0;
   struct list_head *iter, *tmp;
   list_for_each_safe(iter, tmp, &ll_head.head) {
      struct linked_list *cur_node = list_entry(iter, struct linked_list, head);

      unsigned long u_time = 0, s_time = 0;
      int get_cpu_time_success = get_cpu_use(cur_node->pid, 
                                             &cur_node->minor_fault_count,
                                             &cur_node->major_fault_count,
                                             &u_time,
                                             &s_time);

      // check if pid was valid
      if (get_cpu_time_success != 0) {
         // printk(KERN_ALERT "process no longer active AND/OR does not exist: %d", cur_node->pid);
      } else {
         // update sums
         // printk(KERN_INFO "updated PID:%d with min_flt_ct: %ld, mjr_flt_ct: %ld, proc_util: %ld", 
         //          cur_node->pid, cur_node->minor_fault_count, cur_node->major_fault_count, cur_node->process_utilization);
         
         cur_node->process_utilization = u_time + s_time;
         sum_min_flt += cur_node->minor_fault_count;
         sum_mjr_flt += cur_node->major_fault_count;
         sum_proc_util += cur_node->process_utilization;
      }
   }

   spin_unlock(&ll_sp_lock);

   printk(KERN_INFO "updating v_buff[%d] with jiffies: %ld sum_min_flt_ct: %ld, sum_mjr_flt_ct: %ld, sum_proc_util: %ld", 
            v_idx, jiffies, sum_min_flt, sum_mjr_flt, sum_proc_util);
   
   if (v_idx >= VBUFF_SIZE) {
      printk(KERN_INFO "wrapping around v_buff");
      v_idx = 0;
   }

   v_buff[v_idx++] = jiffies;
   v_buff[v_idx++] = sum_min_flt;
   v_buff[v_idx++] = sum_mjr_flt;
   v_buff[v_idx++] = sum_proc_util;

   printk(KERN_INFO "updated v_buff[%d] with jiffies: %ld sum_min_flt_ct: %ld, sum_mjr_flt_ct: %ld, sum_proc_util: %ld", 
            v_idx - 4, v_buff[v_idx - 4], v_buff[v_idx - 3], v_buff[v_idx - 2], v_buff[v_idx - 1]);


   queue_delayed_work(own_wq, &work, msecs_to_jiffies(TIMEOUT_MS));
}


struct linked_list* create_registration_node(pid_t pid) {
   struct linked_list* ll_entry;
   spin_lock(&ll_sp_lock);

   spin_unlock(&ll_sp_lock);

   // if head already initialized, add a node right before ll_head
   printk(KERN_INFO "adding additional node to linked list");
   ll_entry = kzalloc(sizeof(struct linked_list), GFP_KERNEL);
   ll_entry->pid = pid;
   ll_entry->process_utilization = 0;
   ll_entry->major_fault_count = 0;
   ll_entry->linux_task = NULL;

   return ll_entry;
}


void delete_linked_list(void) {
   struct list_head *iter, *tmp;
   
   spin_lock(&ll_sp_lock);
   list_for_each_safe(iter, tmp, &ll_head.head) {
      del_node(iter);
   }
   spin_unlock(&ll_sp_lock);
}


void del_node(struct list_head* node) {
   struct linked_list* cur_node;
   pid_t deleted_pid;

   // del the rest of the struct
   cur_node = list_entry(node, struct linked_list, head);
   
   list_del(node);
   deleted_pid = cur_node->pid;
   kfree(cur_node);
   cur_node = NULL;

   ll_size -= 1;

   // delete wq job, reset wq_job_created flag so that 
   // new wq job can be recreated when new node is registered.
   if (ll_size == 0) {
      printk(KERN_INFO "flushing wq job");
      cancel_delayed_work_sync(&work);
      flush_workqueue(own_wq);
      wq_job_created = false;
   }

   printk(KERN_INFO "deleted node with pid %d", deleted_pid);
}


// mp3_init - Called when module is loaded
int __init mp3_init(void)
{
   int i;
   int cdev_init_err;
   #ifdef DEBUG
   printk(KERN_ALERT "MP3 MODULE LOADING\n");
   #endif
   printk(KERN_INFO "hello world");

   // init MP3 dir
   proc_parent = proc_mkdir(PROC_NAME, NULL);
   if (!proc_parent) {
      printk(KERN_ALERT "Failed to initialize /proc/mp3/ dir");
      return EPERM;
   }

   // init status file in /proc/mp3/
   proc_status = proc_create(STATUS, 0666, proc_parent, &proc_fops);
   if (!proc_status) {
      printk(KERN_ALERT "Failed to initialize status file");
      return EPERM;
   }

   // init linked list
   INIT_LIST_HEAD(&ll_head.head);

   // init workqueue
   own_wq = create_workqueue("own_wq");
   if (!own_wq) {
      printk(KERN_ALERT "Failed to initialize workqueue");
      return EPERM;
   }

   // init virtually contiguous buffer memory (128 x 4kB) using vmalloc
   v_buff = vzalloc(VBUFF_SIZE);
   memset(v_buff, -1, VBUFF_SIZE);
   if (!v_buff) {
      printk(KERN_ALERT "Failed to initialize v_buff");
      return EPERM;
   }

   for (i = 0; i < VBUFF_SIZE; i += PAGE_SIZE) {
      SetPageReserved(vmalloc_to_page((void *)(((unsigned long)v_buff) + i)));
   }

   // init character device driver
   cdev_init_err = register_chrdev_region(dev, 1, "cdev_driver");
   if (cdev_init_err != 0) {
      printk(KERN_ALERT "Failed to initialize cdev");
      return cdev_init_err;
   }
   cdev_init(&cdev, &cdev_fops);
   cdev_add(&cdev, dev, 1);


   printk(KERN_ALERT "MP3 MODULE LOADED\n");
   return 0;
}

// mp3_exit - Called when module is unloaded
void __exit mp3_exit(void)
{
   int i;
   #ifdef DEBUG
   printk(KERN_ALERT "MP3 MODULE UNLOADING\n");
   #endif

   printk(KERN_INFO "destroying workqueue");
   destroy_workqueue(own_wq);

   printk(KERN_INFO "removing /proc/mp3/status file");
   remove_proc_entry(STATUS, proc_parent);
   printk(KERN_INFO "removing /proc/mp3/ dir");
   remove_proc_entry(PROC_NAME, NULL);

   printk(KERN_INFO "deleting linked list");
   delete_linked_list();

   printk(KERN_INFO "deleting virtual memory buffer");

   // for (i = 0; i < VBUFF_SIZE; i += 1) {
   //    printk(KERN_INFO "%ld", v_buff[i]);
   // }

   for (i = 0; i < VBUFF_SIZE; i += PAGE_SIZE) {
      ClearPageReserved(vmalloc_to_page((void *)(((unsigned long)v_buff) + i)));
   }

   vfree(v_buff);

   printk(KERN_INFO "deleting cdev");
   cdev_del(&cdev);
   unregister_chrdev_region(dev, 1);

   printk(KERN_ALERT "MP3 MODULE UNLOADED\n");
}

// Register init and exit funtions
module_init(mp3_init);
module_exit(mp3_exit);
