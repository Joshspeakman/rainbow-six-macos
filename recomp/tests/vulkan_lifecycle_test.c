#include "../runtime/win32/vulkan.c"
#include <assert.h>
uint8_t *G_MEM; int w32_protect_memory;
static uintptr_t serial=0x100000;
static unsigned freed,destroyed;
static VkResult alloc_stub(VkDevice d,const VkCommandBufferAllocateInfo *a,VkCommandBuffer *out){
 (void)d;for(uint32_t k=0;k<a->commandBufferCount;k++)out[k]=(VkCommandBuffer)(serial+=16);return VK_SUCCESS;
}
static void destroy_stub(VkDevice d,VkCommandPool p,const VkAllocationCallbacks *a){(void)d;(void)p;(void)a;destroyed++;}
static void device_destroy_stub(VkDevice d,const VkAllocationCallbacks *a){(void)d;(void)a;}
static void free_stub(VkDevice d,VkCommandPool p,uint32_t n,const VkCommandBuffer *cb){(void)d;(void)p;(void)cb;freed+=n;}
static unsigned live(void){unsigned n=0;for(int i=0;i<NH;i++)n+=htab[i].h!=0;return n;}
static uint32_t device,info=0x20000,stack=0x30000,output=0x40000;
static uint32_t allocate(unsigned pool,unsigned count){
 uint32_t ai[]={VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,0,pool,0,VK_COMMAND_BUFFER_LEVEL_PRIMARY,count};memcpy(G_MEM+info,ai,sizeof ai);
 Ctx c={.esp=stack};rt_w32(G_MEM,stack+4,device);rt_w32(G_MEM,stack+8,info);rt_w32(G_MEM,stack+12,output);
 imp_vulkan_1_vkAllocateCommandBuffers(&c);return c.eax;
}
static void destroy(unsigned pool){
 Ctx c={.esp=stack};rt_w32(G_MEM,stack+4,device);rt_w32(G_MEM,stack+8,pool);rt_w32(G_MEM,stack+12,0);rt_w32(G_MEM,stack+16,0);imp_vulkan_1_vkDestroyCommandPool(&c);
}
int main(void){
 G_MEM=mmap(0,1ull<<32,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON|MAP_NORESERVE,-1,0);assert(G_MEM!=MAP_FAILED);
 w32_process_heap=heap_create(0,0);device=vkh_guest((void *)0x8000);assert(device);
 vkp.vkAllocateCommandBuffers=alloc_stub;vkp.vkDestroyCommandPool=destroy_stub;vkp.vkFreeCommandBuffers=free_stub;
 for(unsigned i=0;i<10000;i++){
  assert(allocate(i+1,2)==VK_SUCCESS);uint32_t g=rt_r32(G_MEM,output);assert(vkh_host(g));destroy(i+1);
  assert(live()==1&&heap_size(w32_process_heap,g)==0xffffffffu);
 }
 assert(allocate(1,1)==VK_SUCCESS);uint32_t first=rt_r32(G_MEM,output);
 assert(allocate(2,1)==VK_SUCCESS);uint32_t second=rt_r32(G_MEM,output);
 destroy(1);assert(live()==2&&vkh_host(second));assert(!vkh_host(first));destroy(2);
 /* Partial mapping failure must roll back the whole host allocation. */
 for(unsigned i=0;i<NH-2;i++)assert(vkh_guest((void *)(uintptr_t)(0x4000000+16*i)));
 assert(live()==NH-1);assert(allocate(3,2)==(uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY);
 assert(freed==2&&live()==NH-1&&!rt_r32(G_MEM,output)&&!rt_r32(G_MEM,output+4));
 for(unsigned i=0;i<NH-2;i++)vkh_forget((void *)(uintptr_t)(0x4000000+16*i));
 assert(live()==1&&allocate(4,1)==VK_SUCCESS);
 Ctx c={.esp=stack};rt_w32(G_MEM,stack+4,device);rt_w32(G_MEM,stack+8,4);rt_w32(G_MEM,stack+12,0);rt_w32(G_MEM,stack+16,1);rt_w32(G_MEM,stack+20,output);imp_vulkan_1_vkFreeCommandBuffers(&c);
 assert(live()==1&&freed==3);destroy(4);
 /* Identical pool values on separate devices must not share ownership. */
 uint32_t original_device=device;assert(allocate(5,1)==VK_SUCCESS);
 device=vkh_guest((void *)0x9000);assert(device&&allocate(5,1)==VK_SUCCESS);
 uint32_t survivor=rt_r32(G_MEM,output);device=original_device;destroy(5);
 assert(live()==3&&vkh_host(survivor));
 device=vkh_guest((void *)0x9000);vkp.vkDestroyDevice=device_destroy_stub;
 c.esp=stack;rt_w32(G_MEM,stack+4,device);rt_w32(G_MEM,stack+8,0);imp_vulkan_1_vkDestroyDevice(&c);
 assert(live()==1);device=original_device;
 puts("Vulkan: pool ownership, explicit free, 10000 pool cycles and exhaustion rollback passed");
}
