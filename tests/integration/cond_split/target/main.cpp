struct Ped { int a, b, c; int state; };
void g(int v);

void Check(Ped* p)
{
    if (p->a)
    {
        if (p->b)
            g(1);
    }
}
