struct Ped { int a, b, c; int state; int get_a() { return a; } };
void g(int v);

void Clear(Ped* p)
{
    p->a = 0;
    p->b = 0;
}
