
// Copyright (C) Force67 2019

#include <cstdio>
#include <loader/SELFLoader.h>
#include <kernel/VMA.h>	

#define SHT_NULL 0
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_HASH 5
#define SHT_DYNAMIC 6
#define SHT_NOTE 7
#define SHT_NOBITS 8
#define SHT_REL 9
#define SHT_SHLIB 10
#define SHT_DYNSYM 11
#define SHT_LOPROC 0x70000000
#define SHT_HIPROC 0x7FFFFFFF
#define SHT_LOUSER 0x80000000
#define SHT_HIUSER 0xFFFFFFFF

// Segment Types
#define PT_NULL 0
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_INTERP 3
#define PT_NOTE 4
#define PT_SHLIB 5
#define PT_PHDR 6
#define PT_TLS  7
#define PT_LOPROC 0x70000000
#define PT_HIPROC 0x7FFFFFFF

#define PT_SCE_DYNLIBDATA 0x61000000
#define PT_SCE_PROCPARAM 0x61000001
#define PT_SCE_MODULEPARAM 0x61000002
#define PT_SCE_RELRO 0x61000010
#define PT_SCE_COMMENT 0x6FFFFF00
#define PT_SCE_VERSION 0x6FFFFF01
#define PT_GNU_EH_FRAME 0x6474E550

// Segment Memory Protection Flags
#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4
#define PF_MASKPROC 0xF0000000

void FormatFlag(uint32_t flag) {
	std::printf("R:%d,W:%d,X:%d\n", 
		(bool)flag & PF_R,
		(bool)flag & PF_W,
		(bool)flag & PF_X);
}

enum ElfType {
	ET_NONE = 0,
	ET_REL = 1,
	ET_EXEC = 2,
	ET_DYN = 3,
	ET_CORE = 4,
	ET_LOPROC = 0xFF00,
	ET_HIPROC = 0xFFFF,
};

namespace loaders
{
	SELF_Loader::SELF_Loader(uint8_t *d) :
		AppLoader(d)
	{}

	FileType SELF_Loader::IdentifyType(const uint8_t* data)
	{
		SELFHeader* hdr = (SELFHeader*)data;
		if (hdr->magic == SELF_MAGIC && hdr->contentType == SELFContentType::SELF) {
			std::puts("[+] Identified file as Signed ELF (SELF)");
			return FileType::SELF;
		}

		return FileType::UNKNOWN;
	}

	LoadErrorCode SELF_Loader::Load(krnl::VMAccessMgr& vma)
	{
		auto* hdr = GetOffset<SELFHeader>(0);

		auto* tables = GetOffset<SELFSegmentTable>(sizeof(SELFHeader));
		for (int i = 0; i < hdr->numSegments; i++) {
			auto* p = &tables[i];

			if (p->fileSize != p->memSize) {
				std::puts("Fatal: we cannot handle dec yet!");
				return LoadErrorCode::BADARCH;
			}

#if 1
			auto flag_set = [&](SegFlags flag) -> bool {
				return p->flags & flag != 0;
			};

			printf("SCESeg:%d | Size: %llu | Offset: 0x%llx | SegID: %d\n",
				i,
				p->fileSize,
				p->offset,
				p->Id());
#endif
		}

		// TODO: find a better way :D
		elf = GetOffset<ELFHeader>(sizeof(SELFHeader) + (sizeof(SELFSegmentTable) * hdr->numSegments));

		if (*GetElfOfs<uint32_t>(0) != 0x464C457F)
			return LoadErrorCode::BADMAGIC;

		// phoff should be 0x40 relative to the elf header
		segments = GetElfOfs<ELFPgHeader>(elf->phoff);

		// section headers are always missing
		//if (elf->shoff == 0)
		//	std::puts("section header table missing!");

		// dont even ask
		size_t offset = sizeof(ELFHeader) + (elf->phnum * sizeof(ELFPgHeader));
		offset += 15; offset &= ~15; // align

		SCESpecial* special = GetElfOfs<SCESpecial>(offset + 7);
		std::printf("SCE special located at offset %llx, %x\n", ((uintptr_t)special - (uintptr_t)data), (uint8_t)special->authId);

		if (MapSegments(vma))
			return LoadErrorCode::SUCCESS;

		return LoadErrorCode::UNKNOWN;
	}

	bool SELF_Loader::MapSegments(krnl::VMAccessMgr& vma)
	{
		// count total segment size we are interested in
		uint32_t total_size = 0;
		for (uint16_t i = 0; i < elf->phnum; ++i) {
			const auto* p = &segments[i];
			if (p->type == PT_LOAD || p->type == PT_SCE_RELRO) {
				total_size += (p->memsz + 0xFFF) & ~0xFFF;
			}
		}

		// reserve kernel memory
		void* mem = vma.AllocateSeg(total_size);
		std::printf("[+] Allocating  %s @%p <%d bytes>\n", TypeToString(), mem, total_size);

		// process segments
		uint32_t offset_hack = 0;
		for (uint16_t i = 0; i < elf->phnum; ++i) {

			const auto* p = &segments[i];

			uint32_t hack = offset_hack;
			offset_hack += p->filesz;

			// ignore info segments
			//if (p->filesz == 0)
			//	continue;

			//if (p->type == PT_LOAD) {
				std::printf("ELF SEG:%d, %llu (%llx) ELF TYPE %s, VIRT %p, PHYS %p, FILESIZE %lld, VSIZE %lld, HEADER OFS %p, OFFSET HACK %x\n",
					(int)i, (uint32_t)p->offset - p->align, (uint32_t)p->offset - p->align,
					SecTypeToStr(p->type),
					(void*)p->vaddr,
					(void*)p->paddr,
					p->filesz,
					p->memsz,
					(void*)((uintptr_t)p - (uintptr_t)data), hack);
				//FormatFlag(p->flags);
			//}

			switch (p->type) {
			case PT_DYNAMIC: {
				SetupDynamics(p);
			} break;
			case PT_TLS: {

				// if the application uses TLS...
				// TODO: check redudant..
				if (p->filesz > 0 && p->offset != 0)
					SetupTLS(p);
			} break;

			}
		}

		return true;
	}

	void SELF_Loader::SetupDynamics(const ELFPgHeader* pg)
	{
		/*auto* dynamics = GetOffset<ELFDyn>(pg->offset);
		for (uint32_t i = 0; i < pg->filesz / sizeof(ELFDyn); i++) {

			const auto* p = &dynamics[i];
			std::printf("%d dyamics found, dynamic type %x\n", i, p->tag);
			
		}*/
	}

	void SELF_Loader::SetupTLS(const ELFPgHeader* pg)
	{
		//TODO: allocate TLS memory
	}

	const char* SELF_Loader::TypeToString()
	{
		switch (elf->type) {
		case ET_SCE_EXEC: return "Executable";
		case ET_SCE_DYNEXEC: return "Main module";
		case ET_SCE_RELEXEC: return "Relocatable PRX";
		case ET_SCE_STUBLIB: return "Stub library";
		case ET_SCE_DYNAMIC: return "Dynamic PRX";
		default: return "Unknown";
		}
	}

	const char* SELF_Loader::SecTypeToStr(uint32_t type)
	{
#define AS_STR(idx)                             \
    if (type == idx)                   \
        return #idx;

			AS_STR(PT_LOAD)
			AS_STR(PT_DYNAMIC)
			AS_STR(PT_INTERP)
			AS_STR(PT_NOTE)
			AS_STR(PT_SHLIB)
			AS_STR(PT_PHDR)
			AS_STR(PT_TLS)
			AS_STR(PT_LOPROC)
			AS_STR(PT_HIPROC)
			AS_STR(PT_SCE_DYNLIBDATA)
			AS_STR(PT_SCE_PROCPARAM)
			AS_STR(PT_SCE_MODULEPARAM)
			AS_STR(PT_SCE_RELRO)
			AS_STR(PT_SCE_COMMENT)
			AS_STR(PT_SCE_VERSION)
			AS_STR(PT_GNU_EH_FRAME)

#undef AS_STR

			return "Unknown";
	}

	LoadErrorCode SELF_Loader::Unload()
	{
		return LoadErrorCode::UNKNOWN;
	}
}