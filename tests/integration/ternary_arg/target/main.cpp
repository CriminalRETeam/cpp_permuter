struct Ped { int a, b, c; int state; };
void g(int v);

void Notify(Ped* p)
{
    if (p->state == 0)
    {
        g(1);
    }
    else
    {
        g(0);
    }
}
