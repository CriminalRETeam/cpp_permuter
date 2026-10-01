struct Ped { int a, b, c; int state; int get_a() { return a; } };
void g(int v);

void Run(Ped* p)
{
    g(p->c);
    g(p->a);
    g(p->b);
    p->state = 2;
}
