
__kernel void test_global_data(__global const uint8* in, __global uint8* out)
{

	__local uint8 buffer[32];

	buffer[get_global_id(0)] = in[get_global_id(0)];

	//makes sure, the buffer is not optimized away
	vc4cl_mutex_lock();
	vc4cl_dma_read(buffer +get_global_id(0));
	vc4cl_mutex_unlock();
///*
	barrier(CLK_LOCAL_MEM_FENCE);
	if(get_global_id(0) > 0)
		buffer[get_global_id(0)] += buffer[get_global_id(0) - 1];
	barrier(CLK_LOCAL_MEM_FENCE);
//*/
	out[get_global_id(0)] = buffer[get_global_id(0)];
}
