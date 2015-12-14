#include "level3.hh"

static bool compare_address(Mapped first, Mapped second)
{
        char* first_addr = (char*)first.mapped_begin;
        char* second_addr = (char*)second.mapped_begin;
        return first_addr < second_addr;
}

bool Mapped::area_contains(unsigned long addr) const
{
        int ret = (addr < mapped_begin + mapped_length) && addr >= mapped_begin;
        return ret;
}


// ###############################################


bool Tracker::of_interest(int syscall) const
{
        return syscall == MMAP_SYSCALL || syscall == MREMAP_SYSCALL
                || syscall == MUNMAP_SYSCALL || syscall == MPROTECT_SYSCALL
                || syscall == BRK_SYSCALL;
}

std::list<Mapped>::iterator Tracker::get_mapped(unsigned long addr)
{
        for (auto it = mapped_areas.begin(); it != mapped_areas.end(); it++)
                if(it->area_contains(addr))
                        return it;
        return mapped_areas.end();
}


int Tracker::handle_mprotect(int syscall, Breaker& b, void* bp)
{
        print_syscall(pid, syscall);
        struct user_regs_struct regs;
        ptrace(PTRACE_GETREGS, pid, NULL, &regs);
        long retval = b.handle_bp(bp, false);
        print_retval(pid, syscall);

        if (retval < 0)
                return retval;

        auto it = get_mapped(regs.rdi);
        if (it == mapped_areas.end())
                return NOT_FOUND;

        long tmp = reinterpret_cast<long>(bp) - it->mapped_begin;
        regs.rsi -= tmp;

        if (regs.rsi > 0)
                regs.rsi = 0;

        it->mapped_protections = regs.rdx;
        for (unsigned i = 0; i < regs.rsi / PAGE_SIZE; ++i)
        {
                it = std::next(it);
                it->mapped_protections = regs.rdx;
        }

        return retval;
}

int Tracker::handle_mremap(int syscall, Breaker& b, void* bp)
{
        print_syscall(pid, syscall);
        struct user_regs_struct regs;
        ptrace(PTRACE_GETREGS, pid, NULL, &regs);
        long retval = b.handle_bp(bp, false);
        print_retval(pid, syscall);

        if ((void*) retval == MAP_FAILED)
                return retval;

        auto it = get_mapped(regs.rdi);
        if (it == mapped_areas.end())
                return NOT_FOUND;

        print_mapped_areas();

        if ((unsigned long)retval != it->mapped_begin)
        {
                printf("NOT MOVE\n");
                it->mapped_begin = retval;
                it->mapped_length = regs.rdx;
                tail_remove(it, regs.rsi / regs.rdx);
        }

        // Old size == New size
        else if(regs.rsi == regs.rdx)
                return retval;

        // Old size > New size <==> Shrinking
        else if (regs.rsi > regs.rdx)
        {
                printf("Shrink\n");
                it->mapped_length = regs.rdx;
                auto tmp = regs.rsi /  regs.rdx;
                tail_remove(it, tmp);
        }

        else
        {
                printf("EXPAND\n");
                unsigned i;
                for (i = 0; i < regs.rdx / PAGE_SIZE; ++i)
                {
                        long addr = retval + i * PAGE_SIZE;
                        mapped_areas.push_back(Mapped(addr, PAGE_SIZE, it->mapped_protections, id_inc++));
                }

                if (regs.rdx % PAGE_SIZE)
                {
                        long addr = retval + i * PAGE_SIZE;
                        long len = regs.rdx % PAGE_SIZE;
                        mapped_areas.push_back(Mapped(addr, len, it->mapped_protections, id_inc++));
                }

        }
        mapped_areas.sort(compare_address);
        return retval;
}

int Tracker::handle_mmap(int syscall, Breaker& b, void* bp)
{
        print_syscall(pid, syscall);
        struct user_regs_struct regs;
        ptrace(PTRACE_GETREGS, pid, NULL, &regs);
        long retval = b.handle_bp(bp, false);
        print_retval(pid, syscall);

        if ((void*) retval == MAP_FAILED)
                return retval;

        if ((regs.r10 & MAP_SHARED) || !(regs.r10 & MAP_ANONYMOUS))
                return retval;

        unsigned i = 0;
        for (i = 0; i < regs.rsi / PAGE_SIZE; ++i)
        {
                long addr = retval + i * PAGE_SIZE;
                mapped_areas.push_back(Mapped(addr, PAGE_SIZE, regs.rdx, id_inc++));
        }

        if (regs.rsi % PAGE_SIZE)
        {
                long addr = retval + i * PAGE_SIZE;
                long len = regs.rsi % PAGE_SIZE;
                mapped_areas.push_back(Mapped(addr, len, regs.rdx, id_inc++));
        }

        mapped_areas.sort(compare_address);

        return retval;
}

int Tracker::handle_brk(int syscall, Breaker& b, void* bp)
{
        static int origin_set = 0;

        print_syscall(pid, syscall);
        struct user_regs_struct regs;
        ptrace(PTRACE_GETREGS, pid, NULL, &regs);
        long retval = b.handle_bp(bp, false);
        print_retval(pid, syscall);

        if (retval < 0)
                return 0;

        if (!origin_set)
        {
                origin_set = 1;
                origin_program_break = (void*)retval;
        }
        else
                actual_program_break = (void*)retval;

        return 0;

}

void Tracker::tail_remove(std::list<Mapped>::iterator it, int iteration)
{
        printf("Tail\n");
        if (iteration <= 0
            || (std::next(it) != mapped_areas.end()))
                return;

        tail_remove(std::next(it), iteration - 1);
        mapped_areas.erase(it);
}

int Tracker::handle_munmap(int syscall, Breaker& b, void* bp)
{
        print_syscall(pid, syscall);
        struct user_regs_struct regs;
        ptrace(PTRACE_GETREGS, pid, NULL, &regs);
        long retval = b.handle_bp(bp, false);
        print_retval(pid, syscall);

        print_errno();
        if (retval < 0)
                return retval;
        auto it = get_mapped(regs.rdi);
        if (it == mapped_areas.end())
                return NOT_FOUND;

        long tmp = reinterpret_cast<long>(bp) - it->mapped_begin;
        long tmp2 = regs.rsi;
        tmp2 -= tmp;

        if (tmp2 < 0)
                tmp2 = 0;

        tail_remove(it, tmp2 / PAGE_SIZE + 1);
        mapped_areas.erase(it);
        mapped_areas.sort(compare_address);
        return retval;
}

int Tracker::handle_syscall(int syscall, Breaker& b, void* bp)
{
        switch (syscall)
        {
                case MMAP_SYSCALL:
                        return handle_mmap(syscall, b, bp);
                case MUNMAP_SYSCALL:
                        return handle_munmap(syscall, b, bp);
                case MPROTECT_SYSCALL:
                        return handle_mprotect(syscall, b, bp);
                case MREMAP_SYSCALL:
                        return handle_mremap(syscall, b, bp);
                case BRK_SYSCALL:
                        return handle_brk(syscall, b, bp);
                default:
                        return b.handle_bp(bp, false);

        }

        return NO_SYSCALL;
}

void Tracker::print_mapped_areas() const
{
        printf("Old process break %p\n", origin_program_break);
        printf("Actual process break %p\n", actual_program_break);
        for (auto it = mapped_areas.begin(); it != mapped_areas.end(); it++)
        {
                fprintf(OUT, "Mapped area #%d\n", it->id);
                fprintf(OUT, "\tBegins:\t%p\n", (void*)it->mapped_begin);
                fprintf(OUT, "\tLength:\t%ld\n", it->mapped_length);
                fprintf(OUT, "\tEnds  :\t%p\n", (char*)it->mapped_begin
                        + it->mapped_length);
                fprintf(OUT, "\tProt  :\t%ld\n\n", it->mapped_protections);
        }
}