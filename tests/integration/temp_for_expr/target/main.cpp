struct Ped { int a, b, c; int state; int get_a() { return a; } };
void g(int v);

void Send(Ped* p)
{
    char tmp = (char)p->a;
    g(tmp);
}
