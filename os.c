#include "os.h"
#include "mxx.h"

void *
os_allocate_block(
HANDLE handleProcess,
uint32_t blockSizeCb,
uint32_t Protect
)
/*
*	���ڴ��������ͷ�
*	Size[_In_]		�����ڴ���С
*
*	RET				���ر������ύ�������ڴ��ַָ��
*      					��������ʧ�ܷ���NULL
*
*	����Ҫ�������ڴ泤��Ϊ PAGE_SIZE ����
*	���������ڽϴ����ڴ����룬 �ÿ��ڴ�������������Ӧ���� PAGE_SIZE ��С
*  ����������Ⱥ� PAGE_SIZE �����룬 ���Բ�ȡ�������룬 Ȼ������Ĵ�ʩ
*	С���ڴ�ʹ������������ÿ��ܻ�Ӱ������
*	ʹ���������������ڴ�飬 Ӧ����ʽ���� PubFreeMemoryBlock �����ͷ�
*/
{
	void *		MemoryBlock;

	//
	// ���뱣֤���볤����ҳ����
	//
	if ( !os_is_page_aligned( blockSizeCb ) ) {
		return NULL;
	}

	if ( !handleProcess || 0 == blockSizeCb ) {
		return NULL;
	}

	__try {
		MemoryBlock = VirtualAllocEx(
			handleProcess,
			NULL,
			blockSizeCb,
			MEM_RESERVE | MEM_COMMIT,
			Protect
			);
		if ( NULL == MemoryBlock ) {
			nis_call_ecr("[nshost.os.os_allocate_block] failed to call VirtualAlloc, error:%u", GetLastError() );
		}
	} __except ( EXCEPTION_EXECUTE_HANDLER )
	{
		MemoryBlock = NULL;
		nis_call_ecr("[nshost.os.os_allocate_block] failed to allocate virtual memory block, process handle:0x%08X, size:%u, exception code:0%08X",
			handleProcess, blockSizeCb, GetExceptionCode() );
	}

	return MemoryBlock;
}

VOID
os_free_memory_block( void * MemoryBlock ) {
	if ( NULL != MemoryBlock ) {
		__try {
			VirtualFree( MemoryBlock, 0, MEM_RELEASE );
		} __except ( EXCEPTION_EXECUTE_HANDLER )
		{
			nis_call_ecr("[nshost.os.os_free_memory_block] failed to allocate virtual memory block, exception code:0%08X", GetExceptionCode() );
		}
	}
}

int
os_unlock_and_free_virtual_pages(
 void * MemoryBlock,
 uint32_t Size
)
{
	if ( NULL != MemoryBlock ) {
		VirtualUnlock( MemoryBlock, Size );
		os_free_memory_block( MemoryBlock );
		return TRUE;
	}

	return FALSE;
}

/*++
������߽�ָ���ڴ������Ϊ�Ƿ�Ҳ����
pMemoryBlock[_In_]		��Ҫ����Ϊ�Ƿ�ҳ�ص������ڴ��ַ, �������ΪNULL, ������һƬ�µĵ�ַ
Size[_In_]					��Ҫ������������ڴ���С�� ����ҳ����
RET						�����ɹ���������������������Ļ�����ָ�룬 ʧ�ܷ��� NULL
--*/
void *
os_lock_virtual_pages( void * MemoryBlock, uint32_t Size ) {
	SIZE_T MinimumWorkingSetSize;
	SIZE_T MaximumWorkingSetSize;
	int Successful;
	HANDLE handleProcess;
	uint32_t errorCode;
	int LoopFlag;

	handleProcess = INVALID_HANDLE_VALUE;
	LoopFlag = TRUE;

	//
	// �������ʹ�ÿտ飬 ����ϣ����������һ���¿�
	// ���������ʹ�ÿտ飬 ��Ӧ���жϴ����Ĵ�С�Ƿ�ҳ����
	//
	if ( NULL == MemoryBlock ) {
		if ( NULL == ( MemoryBlock = os_allocate_block(GetCurrentProcess(), Size, PAGE_READWRITE) ) ) {
			return NULL;
		}
	} else {
		if ( !os_is_page_aligned( Size ) ) {
			return NULL;
		}
	}

	do {
		Successful = VirtualLock( MemoryBlock, Size );
		if ( Successful ) {
			break;
		}

		//
		// ����ǹ��������㵼�µ����������ڴ浽����ҳʧ��
		// ��ʱӦ�õ������̵Ĺ������ռ�����
		// ������������� ֱ���˳�
		//
		errorCode = GetLastError();
		if ( ERROR_WORKING_SET_QUOTA != errorCode ) {
			//
			// �������Ȩ��ʧ�ܣ� ������ pMemoryBlock ���ⲿ����, �����Ǳ����������ڴ棬 ���Ҹ�Ƭ�ڴ�û�������ύ
			// ����ֻ��Ҫ�����ύ��Ƭ�����ڴ棬 Ȼ������ִ�м���
			// Ϊ�˱�֤���ᷢ����ѭ���� �����Ĳ���ִֻ��һ��
			//
			if ( ERROR_NOACCESS == errorCode && LoopFlag ) {
				__try {
					MemoryBlock = VirtualAlloc(
						MemoryBlock,
						Size,
						MEM_COMMIT,
						PAGE_READWRITE
						);
					if ( NULL == MemoryBlock ) {
						nis_call_ecr("[nshost.os.os_lock_virtual_pages] failed VirtualAlloc, code:0x%08X", GetLastError() );
						break;
					}
				} __except ( EXCEPTION_EXECUTE_HANDLER ) {
					nis_call_ecr("[nshost.os.os_lock_virtual_pages] failed to allocate virtual memory block, error code:0x%08X", GetExceptionCode() );
					break;
				}

				LoopFlag = FALSE;
				continue;
			}

			break;
		}

		//
		// ����Ѿ��������������ռ䣬 ��Ȼ�޷����ڴ��������Ƿ�Ҳ�����
		// ��ʱ����ֱ��ʧ��
		// ����ʹ�õ�ǰ���̾���Ƿ�Ϊ��Ч�� ��Ϊ�жϵ��ε���������������
		//
		if ( INVALID_HANDLE_VALUE == handleProcess ) {

			//
			// �� :
			// PROCESS_QUERY_INFORMATION(��ѯ���������ڴ���Ϣ) 
			// PROCESS_SET_QUOTA (���ù��������)
			// �ķ�ʽ�򿪵�ǰ���̾��
			//
			handleProcess = OpenProcess(
				PROCESS_QUERY_INFORMATION | PROCESS_SET_QUOTA | PROCESS_VM_READ,
				FALSE,
				GetCurrentProcessId()
				);
			if ( INVALID_HANDLE_VALUE == handleProcess ) {
				nis_call_ecr("[nshost.os.os_lock_virtual_pages] failed OpenProcess, code:0x%08X", GetLastError() );
				break;
			}
		}

		//
		// ����������Ҫ�鿴��ǰ���̵Ĺ�������С�� ����ȷ���Ƿ����
		//
		Successful = GetProcessWorkingSetSize(
			handleProcess,
			&MinimumWorkingSetSize,
			&MaximumWorkingSetSize
			);
		if ( !Successful ) {
			nis_call_ecr("[nshost.os.os_lock_virtual_pages] failed GetProcessWorkingSetSize, code:0x%08X", GetLastError() );
			break;
		}

		//
		// ������������С�� Ҫ�����ù�������СΪ��
		// ����������С�����й�������С֮��
		//
		MinimumWorkingSetSize += Size;
		MaximumWorkingSetSize += Size;

		Successful = SetProcessWorkingSetSize(
			handleProcess,
			MinimumWorkingSetSize,
			MaximumWorkingSetSize
			);
		if ( !Successful ) {
			nis_call_ecr("[nshost.os.os_lock_virtual_pages] failed SetProcessWorkingSetSize, code:0x%08X", GetLastError() );
			break;
		}

	} while ( TRUE );

	if ( INVALID_HANDLE_VALUE != handleProcess ) {
		CloseHandle( handleProcess );
		handleProcess = INVALID_HANDLE_VALUE;
	}

	if ( !Successful ) {
		__try {
			VirtualFree( MemoryBlock, 0, MEM_RELEASE );
		} __except ( EXCEPTION_EXECUTE_HANDLER ) {
			;
		}

		MemoryBlock = NULL;
	}

	return MemoryBlock;
}