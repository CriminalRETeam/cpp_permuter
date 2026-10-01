struct Ped { int a, b, c; int state; int get_a() { return a; } };
void g(int v);

void Send(Ped* p)
{
    g((char)p->a);
}
