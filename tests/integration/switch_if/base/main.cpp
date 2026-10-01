struct Ped { int a, b, c; int state; };
void g(int v);

void OnCommand(Ped* p, int notify)
{
    if (notify == 1)
    {
        g(p->a);
    }
}
