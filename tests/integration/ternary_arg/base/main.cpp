struct Ped { int a, b, c; int state; };
void g(int v);

void Notify(Ped* p)
{
    g(p->state == 0 ? 1 : 0);
}
