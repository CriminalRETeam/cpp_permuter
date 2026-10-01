struct Ped { int a, b, c; int state; int get_a() { return a; } };
void g(int v);

int Pick(Ped* p)
{
    switch (p->state)
    {
        case 2:
            return 6;
        case 1:
            return 5;
        default:
            return 0;
    }
}
