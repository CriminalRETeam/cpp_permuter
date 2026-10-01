struct Ped { int a, b, c; int state; int get_a() { return a; } };
void g(int v);

void Pair(Ped* p)
{
    int x = p->a;
    int y = p->b;
    g(x);
    g(y);
}
