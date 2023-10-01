# MP3-PageFaultProfiler

## Case study 1:
<img width="422" alt="case_study_1_work_1_2" src="https://user-images.githubusercontent.com/70923196/202086663-52a562a2-1f93-4826-965f-910327f9d5e7.png"> <img width="402" alt="case_study_1_work_3_4" src="https://user-images.githubusercontent.com/70923196/202086671-ebe7246f-c263-4417-b49e-175b0d5102c1.png">

The graph comparing work processes 1 and 2 displays an asymptotic curve that exponentially increases at first, and slows down in growth as time passes. The same can be said about the graph comparing work processes 3 and 4. The two graphs begin to differ when comparing the page fault rates and completion times of the work processes of the graphs with eachother. The graph comparing work processes 1 and 2 has a cumulative page fault total of 400,000 at jiffy 60,000 + 4.2949e9, while the graph comparing work processes 3 and 4 has a cumulative page fault total of 300,000 at jiffy 9,000 + 4.2949e9. This means that the work process 1,2 graph took more a lot more time to complete and has more total page faults, while the work process 3,4 graph took drastically less time to complete and has less cumulative page faults. The second graph (work processes 3,4) is lesser in these categories because it is testing Random Locality access and Locality-based access, both of which involve accessing memory that is close to other memory. The reason why the second graph (work processes 3,4) is lesser is because it is re-accessing memory already in RAM due to the memory’s locality, and therefore having to page fault less — simply because most requests to get memory are within the memory already loaded into the RAM.

## Case Study 2:
<img width="379" alt="case_study_2" src="https://user-images.githubusercontent.com/70923196/202086683-1f65f0e9-3e21-47ae-8a97-5007705e5869.png">

This graph compares the total process utilization (adjusted for wall clock time) time in five separate scenarios: when the number of concurrent processes running is 1, 5, 11, 21, and 28. Here, we see that, as the number of concurrent processes increases, the cpu utilization increases. This occurs to the point where the CPU begins thrashing, and the total utilization decreases at N=28 concurrent processes running. The completion time of the processes go up drastically as well, as shown by the bars, because the bars display the wall clock time as a denominator. My setup is QEMU, with swap enabled.


## Summary
On init, the kernel module creates a `/proc/mp3` directory is created with a file, `status`, inside. 

```C
int __init mp3_init(void)
```

A virtual buffer and a character device driver are initialized as well. The data structure actually keeping track of each PID's information is a kernel linked list. I implemented it using the kernel linked list API.

```C
v_buff = vzalloc(VBUFF_SIZE);
register_chrdev_region(dev, 1, "cdev_driver");
```

There are two functions that handle Registration and Unregistration of a node. Each function is kicked off by the write callback. When the module recieves any of these commands, their respective functions handle the operations that must be done with the PID. The register function adds a node to my linked list with metadata for the process including minor page fault, major page fault, and process utilization variables. These variables will begin to be filled immediately by my workqueue profiler.

```C
void register_node(struct file * fp, char* buffer, size_t n, loff_t * seek);
void deregister_node(struct file* fp, char* buffer, size_t n, loff_t* seek);
```

When a user tries to add a processto my `/proc/mp3/status` file, my `write_callback` function is triggered. This function is customized to write the user buffer into my internal kernel linked list.

```C
ssize_t write_callback(struct file * fp, const char __user * usr_str, size_t n, loff_t * seek)
```

Any addition to the linked list must be threadsafe:

```C
   mutex_lock(&ll_lock);
   list_add_tail(&ll_entry->head, &ll_head.head);
   mutex_unlock(&ll_lock);
```

When the user tries to read from the `/proc/mp3/status` file, my `read_callback` function is triggered. This function iterates through my internal kernel linked list and displays PIDs in a specific format to the console.

```C
ssize_t read_callback(struct file * fp, char __user * usr_str, size_t n, loff_t * seek)
```

The `read_callback` iterates through the linked list as well:
```C
list_for_each(iter, &ll_head.head) {
     pid_t node_pid = list_entry(iter, struct linked_list, head)->pid;
     long node_user_time = list_entry(iter, struct linked_list, head)->user_time;
     bytes_written += sprintf(buffer + bytes_written, "%d: %ld\n", node_pid, node_user_time);
     printk(KERN_INFO "wrote PID:%d to buffer", node_pid);
}
```

After registration, the nodes' minor/major page fault rates and cpu utilization rates get continuously updated (20 times a second) by my profiler. This profiler consists of a workqueue that gets a job as soon as a node is added to my kernel linked list.

```C
static void workqueue_fn(struct work_struct *w)
```

The virtual buffer (which I made bounded) gets updated with the nodes' summed up metadata, and copied over on user mmap request to a userspace buffer through the use of a character device driver, which is a file (because everything in linux is treated as a file) that can be accessed by both kernel and user spaces.

```C
v_buff[v_idx++] = jiffies;
v_buff[v_idx++] = sum_min_flt;
v_buff[v_idx++] = sum_mjr_flt;
v_buff[v_idx++] = sum_proc_util;
```

The mmap callback on the kernel end handles the mapping of the virtual buffer to a buffer that the user process can view and own. This is a way for the kernel profiling data to be communicated to the requesting process.

```C
static int mmap_cdev_callback(struct file *filp, struct vm_area_struct *vma)
```

On exit, the kernel module deletes the kernel linked list it was using to store PID information. It destroys the workqueue, locks, character device driver, and proc file as well. All other data structures are destroyed.

```C
void __exit mp3_exit(void)
```

### User Interaction With Proc File System
The `/proc` folder refers to a filesystem that is created by Linux on boot, and is used to store information about various system
metadata. This MP was easier than the last because I didn't have to build an entire scheduler, and I was way more familiar with workqueues at this point. My kernel module creates a new sub-directory in the `/proc`
filesystem on initialization, and adds a `status` file to the new subdirectory. The user is able to `cat` my `/proc/status` file to see the contents of the file.
When the `cat` command is called on my status file, the system calls my `read_callback` function. 
I implemented this function to define custom behaviour for the file when it is read. 
This is because I want to display specific information in a singular format. 
I implemented a custom version of this function to write PIDs and the user time of each PID in a specific format. 
The user can send Register and Unregister commands through the userapp. These commands are then handled by different functions.

### Storing Process Information Using Kernel List
I utilized the kernel linked list API to implement a linked list that holds a process PID and a bunch of other metadata.
This implementation allows the user to have custom struct functionality while efficiently utilizing the provided linked list methods. 
I add a node to the list when the user registers a node via the `/proc/mp3/status` file, and I delete a node from the list when a deregister command is recieved or the module is removed from the kernel. 

This function deletes my linked list on exit:
```C
void delete_linked_list(void)
```
