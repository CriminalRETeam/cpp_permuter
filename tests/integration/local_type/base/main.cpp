struct Ped { int a, b, c; int state; int get_a() { return a; } };
void g(int v);

void Count(Ped* p)
{
    int x = p->a;
    if (x > 3)
    {
        g(x);
    }
}
